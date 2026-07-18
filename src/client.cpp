#include <iostream>
#include <vector>
#include <string>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

using namespace std;

static void die(const char* msg) {
    perror(msg);
    abort();
}

static int32_t read_full(int fd, char* buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

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

static int32_t read_res(int fd) {
    char rbuf[4 + 4096];
    if (read_full(fd, rbuf, 4) < 0) return -1;

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > 4096) return -1;

    if (read_full(fd, &rbuf[4], len) < 0) return -1;

    const uint8_t* cur = (const uint8_t*)&rbuf[4];
    const uint8_t* end = cur + len;

    uint32_t nstr = 0;
    memcpy(&nstr, cur, 4); cur += 4;

    cout << "Response (" << nstr << " strings): ";
    for (uint32_t i = 0; i < nstr; i++) {
        uint32_t slen = 0;
        memcpy(&slen, cur, 4); cur += 4;
        cout << string((const char*)cur, slen) << " ";
        cur += slen;
    }
    cout << endl;
    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) < 0) die("connect");

    // Blast multiple pipelined commands concurrently without waiting for sequential blocking answers
    cout << "Blasting 3 pipelined commands..." << endl;
    send_req(fd, { "set", "user", "alex" });
    send_req(fd, { "get", "user" });
    send_req(fd, { "del", "user" });

    // Consume and parse responses back off the network sequence queue
    cout << "Reading replies..." << endl;
    read_res(fd);
    read_res(fd);
    read_res(fd);

    close(fd);
    return 0;
}