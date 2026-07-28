#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
#include <iostream>

using namespace std;

static void msg(const char* msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char* msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

//send the data. if kernel buffer is full try until all data is sent
static int32_t read_full(int fd, char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

//write data if kernel buffer full resend
static int32_t write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// serializes a multi-argument vector list command down into raw network protocol bytes
static int32_t send_req(int fd, const vector<string>& cmd) {
    uint32_t len = 4;
    for (const string& s : cmd) {
        len += 4 + s.size();
    }

    if (len > 4096) {
        return -1;
    }

    vector<char> wbuf(4 + len);
    memcpy(&wbuf[0], &len, 4);

    uint32_t n = (uint32_t)cmd.size();
    memcpy(&wbuf[4], &n, 4);

    size_t cur = 8;
    for (const string& s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf.data(), wbuf.size());
}

const size_t k_max_msg = 32 << 20;   // must match the server frame cap

// mirrors the server tags, kept in sync by hand since the two dont share a header
enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

// prints one tagged value, returns bytes consumed or -1 on a malformed reply
// arrays recurse, each element is a full value so the same parser handles them
static int32_t print_response(const uint8_t* data, size_t size) {
    if (size < 1) {
        msg("bad response");
        return -1;
    }
    switch (data[0]) {
    case TAG_NIL:
        printf("(nil)\n");
        return 1;
    case TAG_ERR: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        int32_t code = 0;
        uint32_t len = 0;
        memcpy(&code, &data[1], 4);
        memcpy(&len, &data[1 + 4], 4);
        if (size < 1 + 8 + len) { msg("bad response"); return -1; }
        printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
        return 1 + 8 + len;
    }
    case TAG_STR: {
        if (size < 1 + 4) { msg("bad response"); return -1; }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        if (size < 1 + 4 + len) { msg("bad response"); return -1; }
        printf("(str) %.*s\n", len, &data[1 + 4]);
        return 1 + 4 + len;
    }
    case TAG_INT: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        int64_t val = 0;
        memcpy(&val, &data[1], 8);
        printf("(int) %lld\n", (long long)val);
        return 1 + 8;
    }
    case TAG_DBL: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        double val = 0;
        memcpy(&val, &data[1], 8);
        printf("(dbl) %g\n", val);
        return 1 + 8;
    }
    case TAG_ARR: {
        if (size < 1 + 4) { msg("bad response"); return -1; }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        printf("(arr) len=%u\n", len);
        size_t bytes = 1 + 4;
        for (uint32_t i = 0; i < len; i++) {
            int32_t rv = print_response(&data[bytes], size - bytes);
            if (rv < 0) return rv;      // malformed element aborts the whole array
            bytes += (size_t)rv;
        }
        printf("(arr) end\n");
        return (int32_t)bytes;
    }
    default:
        msg("bad response");
        return -1;
    }
}

// reads one length prefixed frame then hands the payload to the tag parser
static int32_t read_res(int fd) {
    vector<uint8_t> rbuf(4);
    errno = 0;
    int32_t err = read_full(fd, (char*)rbuf.data(), 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }
    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    rbuf.resize(4 + len);
    err = read_full(fd, (char*)&rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // one frame is exactly one value, leftover bytes mean a framing bug
    int32_t rv = print_response(&rbuf[4], len);
    if (rv < 0) return -1;
    if ((uint32_t)rv != len) {
        msg("bad response");
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    int rv = connect(fd, (const struct sockaddr*)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    if (cmd.empty()) {
        cmd = { "get", "k" }; // default test
    }

    int32_t err = send_req(fd, cmd);
    if (err) {
        close(fd);
    }
    err = read_res(fd);
    if (err) {
        close(fd);
    }


}