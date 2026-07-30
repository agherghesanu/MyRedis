#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#include <math.h>

#include <string>
#include <vector>

#include "hashtable.h"
#include "zset.h"

using namespace std;

// walk back from an embedded member to the struct containing it
#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

static void msg(const char* msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char* msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char* msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

// puts fd in nonblocking mode so read/write return EAGAIN instead of parking the loop
static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 20;

// per client state, buffers persist across poll wakeups since tcp is a byte stream
struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    vector<uint8_t> incoming;
    vector<uint8_t> outgoing;
};

// appends len bytes to the back of buf
static void buf_append(vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// drops n bytes off the front, o(n) memmove per call
static void buf_consume(vector<uint8_t>& buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

// accepts a pending connection and returns fresh state for it, NULL on failure
static Conn* handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return NULL;
    }

    fd_set_nb(connfd);

    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

const size_t k_max_args = 200 * 1000;

// reads a little endian u32 and advances cur, false if it would run past end
static bool read_u32(const uint8_t*& cur, const uint8_t* end, uint32_t& out) {
    if (cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

// reads n raw bytes into out and advances cur, false if it would run past end
static bool read_str(const uint8_t*& cur, const uint8_t* end, size_t n, string& out) {
    if (cur + n > end) return false;
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

// decodes nstr followed by nstr (len, bytes) pairs into out, -1 on malformed input
static int32_t parse_req(const uint8_t* data, size_t size, vector<string>& out) {
    const uint8_t* end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) return -1;
    if (nstr > k_max_args) return -1;

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) return -1;
        out.push_back(string());
        if (!read_str(data, end, len, out.back())) return -1;
    }
    if (data != end) return -1;
    return 0;
}

// every value in a reply is one tag byte then a type specific payload
// nested arrays let one reply carry structured data, not just a flat blob
enum {
    TAG_NIL = 0,   // nothing, like a missing key
    TAG_ERR = 1,   // error code + message
    TAG_STR = 2,   // string, length prefixed
    TAG_INT = 3,   // signed 64 bit int
    TAG_DBL = 4,   // 64 bit double
    TAG_ARR = 5,   // n elements, each a tagged value of its own
};

// error codes carried by a TAG_ERR value
enum {
    ERR_UNKNOWN = 1,   // command not recognised
    ERR_TOO_BIG = 2,   // reply outgrew k_max_msg
    ERR_BAD_TYP = 3,   // command used on a key of the wrong type
    ERR_BAD_ARG = 4,   // argument failed to parse as a number
};

// fixed width appenders, little endian to match the reader on the other side
static void buf_append_u8(vector<uint8_t>& buf, uint8_t data) {
    buf.push_back(data);
}
static void buf_append_u32(vector<uint8_t>& buf, uint32_t data) {
    buf_append(buf, (const uint8_t*)&data, 4);
}
static void buf_append_i64(vector<uint8_t>& buf, int64_t data) {
    buf_append(buf, (const uint8_t*)&data, 8);
}
static void buf_append_dbl(vector<uint8_t>& buf, double data) {
    buf_append(buf, (const uint8_t*)&data, 8);
}

// one out_* per tag, each writes a complete self describing value
static void out_nil(vector<uint8_t>& out) {
    buf_append_u8(out, TAG_NIL);
}
static void out_str(vector<uint8_t>& out, const char* s, size_t size) {
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, (uint32_t)size);
    buf_append(out, (const uint8_t*)s, size);
}
static void out_int(vector<uint8_t>& out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}
static void out_dbl(vector<uint8_t>& out, double val) {
    buf_append_u8(out, TAG_DBL);
    buf_append_dbl(out, val);
}
static void out_err(vector<uint8_t>& out, uint32_t code, const string& msg) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)msg.size());
    buf_append(out, (const uint8_t*)msg.data(), msg.size());
}
// only writes the header, caller must emit exactly n values after this
static void out_arr(vector<uint8_t>& out, uint32_t n) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, n);
}

// for arrays whose length is unknown until the elements are produced
// writes a zero count placeholder and returns where to backfill it later
static size_t out_begin_arr(vector<uint8_t>& out) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, 0);
    return out.size() - 4;
}
// overwrites the placeholder once the real element count is known
static void out_end_arr(vector<uint8_t>& out, size_t ctx, uint32_t n) {
    memcpy(&out[ctx], &n, 4);
}


static struct {
    HMap db;
} g_data;

// a key can hold one of two value kinds tracked by type
enum {
    T_STR = 0,    // plain string in val
    T_ZSET = 1,   // sorted set in zset
};

struct Entry {
    struct HNode node; // embedded hashtable node
    string key;
    uint32_t type = T_STR;
    string val;        // used when type is t_str
    ZSet zset;         // used when type is t_zset
};

// frees a value and its owning entry a zset must drain its members first
static void entry_del(Entry* ent) {
    if (ent->type == T_ZSET) {
        zset_clear(&ent->zset);
    }
    delete ent;
}

// key comparison injected into the table, which only knows HNode
static bool entry_eq(HNode* lhs, HNode* rhs) {
    struct Entry* le = container_of(lhs, struct Entry, node);
    struct Entry* re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

static uint64_t str_hash(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

// looks up cmd[1], emits the value as a string or nil when absent
// probe is a stack Entry, never inserted, caller must fill hcode itself
static void do_get(vector<string>& cmd, vector<uint8_t>& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) return out_nil(out);

    Entry* ent = container_of(node, Entry, node);
    if (ent->type != T_STR) return out_err(out, ERR_BAD_TYP, "not a string");
    return out_str(out, ent->val.data(), ent->val.size());
}

// overwrites cmd[1] in place if present, otherwise heap allocates and inserts
// replies nil either way, set only cares about success not old value
static void do_set(vector<string>& cmd, vector<uint8_t>& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        Entry* ent = container_of(node, Entry, node);
        if (ent->type != T_STR) return out_err(out, ERR_BAD_TYP, "not a string");
        ent->val.swap(cmd[2]);
    }
    else {
        Entry* ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->type = T_STR;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

// unlinks cmd[1] and frees the owning Entry, table only unlinks
// replies 1 if a key was removed, 0 if it was already absent
static void do_del(vector<string>& cmd, vector<uint8_t>& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? 1 : 0);
}

// per node callback, appends one key string to the array being built in arg
static void cb_keys(HNode* node, void* arg) {
    vector<uint8_t>& out = *(vector<uint8_t>*)arg;
    const string& key = container_of(node, Entry, node)->key;
    out_str(out, key.data(), key.size());
}

// dumps every key as a string array, header count comes from hm_size up front
static void do_keys(vector<string>&, vector<uint8_t>& out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void*)&out);
}

// parse a whole string as a double false if any trailing junk or nan
static bool str2dbl(const string& s, double& out) {
    char* endp = nullptr;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !isnan(out);
}

// parse a whole string as a base 10 int64 false if any trailing junk
static bool str2int(const string& s, int64_t& out) {
    char* endp = nullptr;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

// look up a key expected to hold a zset
// a missing key reads as the shared empty set so queries just return nothing
// a wrong type key returns null so the caller can raise an error
static ZSet k_empty_zset;
static ZSet* expect_zset(string& key_str) {
    Entry key;
    key.key.swap(key_str);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) return &k_empty_zset;
    Entry* ent = container_of(node, Entry, node);
    return ent->type == T_ZSET ? &ent->zset : nullptr;
}

// zadd key score name adds or moves a member replies 1 when newly added
static void do_zadd(vector<string>& cmd, vector<uint8_t>& out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) return out_err(out, ERR_BAD_ARG, "expect a number");

    // find the zset for the key or make a fresh one
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);

    Entry* ent = nullptr;
    if (!node) {
        ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->type = T_ZSET;
        hm_insert(&g_data.db, &ent->node);
    }
    else {
        ent = container_of(node, Entry, node);
        if (ent->type != T_ZSET) return out_err(out, ERR_BAD_TYP, "not a zset");
    }

    const string& name = cmd[3];
    bool added = zset_insert(&ent->zset, name.data(), name.size(), score);
    return out_int(out, added ? 1 : 0);
}

// zrem key name removes a member replies 1 when it existed
static void do_zrem(vector<string>& cmd, vector<uint8_t>& out) {
    ZSet* zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYP, "not a zset");

    const string& name = cmd[2];
    ZNode* node = zset_lookup(zset, name.data(), name.size());
    if (node) zset_delete(zset, node);
    return out_int(out, node ? 1 : 0);
}

// zscore key name replies the score as a double or nil when absent
static void do_zscore(vector<string>& cmd, vector<uint8_t>& out) {
    ZSet* zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYP, "not a zset");

    const string& name = cmd[2];
    ZNode* node = zset_lookup(zset, name.data(), name.size());
    if (!node) return out_nil(out);
    return out_dbl(out, node->score);
}

// zquery key score name offset limit
// seeks to the pair score name skips offset members then emits up to limit of
// them as a flat array of name score name score in ascending order
static void do_zquery(vector<string>& cmd, vector<uint8_t>& out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) return out_err(out, ERR_BAD_ARG, "expect a number");
    const string& name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset)) return out_err(out, ERR_BAD_ARG, "expect an int");
    if (!str2int(cmd[5], limit)) return out_err(out, ERR_BAD_ARG, "expect an int");

    ZSet* zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYP, "not a zset");

    if (limit <= 0) return out_arr(out, 0);

    ZNode* znode = zset_seekge(zset, score, name.data(), name.size());
    znode = znode_offset(znode, offset);

    size_t ctx = out_begin_arr(out);
    uint32_t n = 0;                       // members emitted so far
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        znode = znode_offset(znode, +1);
        n++;
    }
    out_end_arr(out, ctx, n * 2);         // two array slots per member
}

// dispatches on cmd[0] and arity, TAG_ERR for anything unrecognised
static void do_request(vector<string>& cmd, vector<uint8_t>& out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    }
    else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    }
    else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    }
    else if (cmd.size() == 1 && cmd[0] == "keys") {
        return do_keys(cmd, out);
    }
    else if (cmd.size() == 4 && cmd[0] == "zadd") {
        return do_zadd(cmd, out);
    }
    else if (cmd.size() == 3 && cmd[0] == "zrem") {
        return do_zrem(cmd, out);
    }
    else if (cmd.size() == 3 && cmd[0] == "zscore") {
        return do_zscore(cmd, out);
    }
    else if (cmd.size() == 6 && cmd[0] == "zquery") {
        return do_zquery(cmd, out);
    }
    else {
        return out_err(out, ERR_UNKNOWN, "unknown command.");
    }
}

// reserves a 4 byte length slot and remembers where it sits
// payload gets written straight after, length backfilled once its known
static void response_begin(vector<uint8_t>& out, size_t& header) {
    header = out.size();
    buf_append_u32(out, 0);   // placeholder, real length filled by response_end
}

static size_t response_size(vector<uint8_t>& out, size_t header) {
    return out.size() - header - 4;
}

// backfills the length prefix, or swaps an oversized reply for an error
static void response_end(vector<uint8_t>& out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) {
        out.resize(header + 4);                 // drop the half written payload
        out_err(out, ERR_TOO_BIG, "response is too big.");
        msg_size = response_size(out, header);
    }
    uint32_t len = (uint32_t)msg_size;
    memcpy(&out[header], &len, 4);
}

// handles one complete request if the buffer holds one, true means try again
// false means either incomplete or fatal, check want_close to tell them apart
static bool try_one_request(Conn* conn) {
    if (conn->incoming.size() < 4) return false;

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    if (4 + len > conn->incoming.size()) return false;

    const uint8_t* request = &conn->incoming[4];
    vector<string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;
    }

    // generate the reply in place, wrapped in its length prefix
    size_t header = 0;
    response_begin(conn->outgoing, header);
    do_request(cmd, conn->outgoing);
    response_end(conn->outgoing, header);

    buf_consume(conn->incoming, 4 + len);
    return true;
}

// flushes what the socket will take, partial writes are normal
// flips back to reading once outgoing is empty
static void handle_write(Conn* conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) return;
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }
    buf_consume(conn->outgoing, (size_t)rv);
    if (conn->outgoing.size() == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

// drains the socket once, processes every complete request in it, rv 0 means eof
// tries an immediate write to skip a poll round trip
static void handle_read(Conn* conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN) return;
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }
    if (rv == 0) {
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while (try_one_request(conn)) {}

    if (conn->outgoing.size() > 0) {
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}

// sets up the listener then runs the event loop
// poll_args is rebuilt each round since per connection interests change
// fd2conn is indexed directly by fd, which the kernel keeps small and reuses
int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);

    int rv = bind(fd, (const sockaddr*)&addr, sizeof(addr));
    if (rv) die("bind()");

    fd_set_nb(fd);

    rv = listen(fd, SOMAXCONN);
    if (rv) die("listen()");

    vector<Conn*> fd2conn;
    vector<struct pollfd> poll_args;

    

    while (true) {
        poll_args.clear();
        struct pollfd pfd = { fd, POLLIN, 0 };
        poll_args.push_back(pfd);   // slot 0 is always the listener

        for (Conn* conn : fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = { conn->fd, POLLERR, 0 };
            if (conn->want_read) pfd.events |= POLLIN;
            if (conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) continue;   // signal, not an error
        if (rv < 0) die("poll");

        if (poll_args[0].revents) {
            if (Conn* conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {   // skip the listener
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) continue;
            Conn* conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) handle_read(conn);
            if (ready & POLLOUT) handle_write(conn);
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }
    }
    return 0;
}