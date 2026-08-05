// concurrent load generator for the redis server
// opens one persistent connection per thread and drives requests at it
// the server is a single event loop so many connections at once is the contention
//
// build and run examples
//   bench -t 16 -n 100000 -w mixed -P 16
//   bench -w zadd -t 8         hammer one sorted set from every thread
//
// flags
//   -t threads          number of client threads and connections
//   -n reqs_per_thread  requests each thread sends
//   -k keyspace         number of distinct keys or members
//   -w workload         get set mixed zadd zquery
//   -P pipeline         outstanding requests per connection
//   -h host  -p port
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>

using namespace std;
using Clock = chrono::steady_clock;

// one shared config filled from argv
struct Config {
    int threads = 8;
    int reqs = 100000;      // per thread
    int keyspace = 1000;
    int pipeline = 16;
    string workload = "mixed";
    string host = "127.0.0.1";
    int port = 1234;
};

static void die(const char* m) {
    perror(m);
    exit(1);
}

// blocking send of the whole buffer retries on short writes
static bool write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return false;
        n -= (size_t)rv; buf += rv;
    }
    return true;
}

// blocking read of exactly n bytes
static bool read_full(int fd, char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return false;
        n -= (size_t)rv; buf += rv;
    }
    return true;
}

// encode a command into the length prefixed request frame the server parses
static void encode(vector<char>& out, const vector<string>& cmd) {
    uint32_t len = 4;
    for (const string& s : cmd) len += 4 + (uint32_t)s.size();
    out.clear();
    out.resize(4 + len);
    memcpy(&out[0], &len, 4);
    uint32_t n = (uint32_t)cmd.size();
    memcpy(&out[4], &n, 4);
    size_t cur = 8;
    for (const string& s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&out[cur], &p, 4);
        memcpy(&out[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
}

// read and discard one reply frame the body content does not matter to the bench
static bool skip_reply(int fd, vector<char>& scratch) {
    char hdr[4];
    if (!read_full(fd, hdr, 4)) return false;
    uint32_t len = 0;
    memcpy(&len, hdr, 4);
    if (scratch.size() < len) scratch.resize(len);
    return read_full(fd, scratch.data(), len);
}

static int dial(const Config& cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // latency not batching
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) die("connect");
    return fd;
}

// pick the command for one request based on the workload and a per thread rng
static void make_cmd(const Config& cfg, mt19937& rng, vector<string>& cmd) {
    int k = (int)(rng() % (uint32_t)cfg.keyspace);
    char key[32];
    cmd.clear();
    if (cfg.workload == "get") {
        snprintf(key, sizeof(key), "k%d", k);
        cmd = { "get", key };
    }
    else if (cfg.workload == "set") {
        snprintf(key, sizeof(key), "k%d", k);
        cmd = { "set", key, "v" };
    }
    else if (cfg.workload == "mixed") {
        snprintf(key, sizeof(key), "k%d", k);
        if (rng() % 100 < 80) cmd = { "get", key };   // 80 percent reads
        else cmd = { "set", key, "v" };
    }
    else if (cfg.workload == "zadd") {
        snprintf(key, sizeof(key), "m%d", k);
        cmd = { "zadd", "zbench", to_string(k), key };  // every thread hits one set
    }
    else if (cfg.workload == "zquery") {
        cmd = { "zquery", "zbench", "-inf", "", "0", "100" };
    }
}

// per thread stats merged at the end
struct Stat {
    vector<double> lat_us;   // one entry per completed request
    long ops = 0;
};

// the client loop keeps pipeline requests in flight and matches replies fifo
// send timestamps queue up so each reply gets its own true latency
static void run_client(const Config& cfg, int tid, Stat* st) {
    int fd = dial(cfg);
    mt19937 rng((uint32_t)(0x9e3779b9u * (tid + 1)));
    st->lat_us.reserve(cfg.reqs);

    vector<char> frame, scratch(4096);
    deque<Clock::time_point> pending;
    int sent = 0, done = 0;
    vector<string> cmd;

    while (done < cfg.reqs) {
        // top up the pipeline
        while (sent < cfg.reqs && (int)pending.size() < cfg.pipeline) {
            make_cmd(cfg, rng, cmd);
            encode(frame, cmd);
            if (!write_all(fd, frame.data(), frame.size())) die("write");
            pending.push_back(Clock::now());
            sent++;
        }
        // collect one reply and time it against its send
        if (!skip_reply(fd, scratch)) die("read");
        auto t = pending.front(); pending.pop_front();
        double us = chrono::duration<double, micro>(Clock::now() - t).count();
        st->lat_us.push_back(us);
        done++;
    }
    close(fd);
    st->ops = done;
}

// fill the keyspace so reads hit and zquery has data to page through
static void setup(const Config& cfg) {
    int fd = dial(cfg);
    vector<char> frame, scratch(4096);
    if (cfg.workload == "zquery" || cfg.workload == "zadd") {
        for (int i = 0; i < cfg.keyspace; i++) {
            vector<string> cmd = { "zadd", "zbench", to_string(i), "m" + to_string(i) };
            encode(frame, cmd);
            write_all(fd, frame.data(), frame.size());
            skip_reply(fd, scratch);
        }
    }
    else {
        for (int i = 0; i < cfg.keyspace; i++) {
            vector<string> cmd = { "set", "k" + to_string(i), "v" };
            encode(frame, cmd);
            write_all(fd, frame.data(), frame.size());
            skip_reply(fd, scratch);
        }
    }
    close(fd);
    printf("setup: populated %d %s\n", cfg.keyspace,
           (cfg.workload == "zquery" || cfg.workload == "zadd") ? "members" : "keys");
}

static double pct(const vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t i = (size_t)(p * (sorted.size() - 1));
    return sorted[i];
}

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i + 1 < argc; i += 2) {
        string a = argv[i], v = argv[i + 1];
        if (a == "-t") cfg.threads = atoi(v.c_str());
        else if (a == "-n") cfg.reqs = atoi(v.c_str());
        else if (a == "-k") cfg.keyspace = atoi(v.c_str());
        else if (a == "-P") cfg.pipeline = atoi(v.c_str());
        else if (a == "-w") cfg.workload = v;
        else if (a == "-h") cfg.host = v;
        else if (a == "-p") cfg.port = atoi(v.c_str());
    }

    printf("bench: workload=%s threads=%d pipeline=%d reqs/thread=%d keyspace=%d\n",
           cfg.workload.c_str(), cfg.threads, cfg.pipeline, cfg.reqs, cfg.keyspace);
    setup(cfg);

    vector<Stat> stats(cfg.threads);
    vector<thread> pool;
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.threads; i++) {
        pool.emplace_back(run_client, cref(cfg), i, &stats[i]);
    }
    for (auto& t : pool) t.join();
    double elapsed = chrono::duration<double>(Clock::now() - t0).count();

    // merge every threads latencies for global percentiles
    vector<double> all;
    long total = 0;
    for (auto& s : stats) {
        total += s.ops;
        all.insert(all.end(), s.lat_us.begin(), s.lat_us.end());
    }
    sort(all.begin(), all.end());
    double sum = 0; for (double x : all) sum += x;

    printf("throughput: %.0f ops/sec  (%ld ops in %.2fs)\n",
           total / elapsed, total, elapsed);
    printf("latency us: avg %.1f  p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f  max %.0f\n",
           all.empty() ? 0 : sum / all.size(),
           pct(all, 0.50), pct(all, 0.90), pct(all, 0.99), pct(all, 0.999),
           all.empty() ? 0 : all.back());
    return 0;
}
