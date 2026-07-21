// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <iostream>

// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

// C++
#include <string>
#include <vector>
#include <map>
#include "conn.h"

using namespace std;

const size_t k_max_msg = 32 << 20; // 32mb buffer

const size_t k_max_args = 200 * 1000;

enum {
	RES_OK = 0,
	RES_ERR = 1,    // error
	RES_NX = 2,     // key not found
};

struct Response {
	uint32_t status = 0;
	vector<uint8_t> data;
};

// Placeholder in-memory database map
static map<string, string> g_data;



static void msg(const char* msg) {
	fprintf(stderr, "%s\n", msg);// sderr prints directly to terminal no buffer
	//better practice for concurrent code
}

static void msg_errno(const char* msg) {
	fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char* msg) {
	fprintf(stderr, "[%d] %s\n", errno, msg);
	abort();
}

// sets up non blocking file descriptor
static void fd_set_nb(int fd) {
	errno = 0; // more robust error handling 
	//reversion to 0 needed bc errno doesnt reset on success
	int flags = fcntl(fd, F_GETFL, 0); // get flags
	if (errno) {
		die("fcntl error");
		return;
	}
	flags |= O_NONBLOCK; // append non blocking flags
	errno = 0;
	(void)fcntl(fd, F_SETFL, flags); //syscall to set flags
	if (errno) {
		die("fcntl error");
	}
}

//append data chunck to vector stream
static void buf_append(Buffer& buf, const uint8_t* data, size_t len) {
	buf.insert(buf.end(), data, data + len);
}

//consume data from the front of the vector stream
static void buf_consume(Buffer& buf, size_t len) {
	buf.erase(buf.begin(), buf.begin() + len);
}


// helper for acception connections from clients
static Conn* handle_accept(int fd) {
	struct sockaddr_in client_addr = {};
	socklen_t addrlen = sizeof(client_addr);
	int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
	if (connfd < 0) {
		msg_errno("accept() error");
		return NULL;
	}
	uint32_t ip = client_addr.sin_addr.s_addr;
	fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
		ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
		ntohs(client_addr.sin_port)
	);

	fd_set_nb(connfd);

	Conn* conn = new Conn();
	conn->fd = connfd;
	conn->want_read = true;
	return conn;
}

//helper to parse strings out of our procol message list
static bool read_u32(const uint8_t*& cur, const uint8_t* end, uint32_t& out) {
	if (cur + 4 > end) {
		return false;
	}
	memcpy(&out, cur, 4);
	cur += 4;

	return true;
}


static bool read_str(const uint8_t*& cur, const uint8_t* end, size_t n, string& out) {
	if (cur + n > end) {
		return false;
	}
	out.assign(cur, cur + n);
	cur += n;
	return true;
}

// parses command arguments: [nstr][len1][str1][len2][str2]...
static int32_t parse_req(const uint8_t* data, size_t size, vector<string>& out) {
	const uint8_t* end = data + size;
	uint32_t nstr = 0;
	if (!read_u32(data, end, nstr)) {
		return -1;
	}
	if (nstr > k_max_args) {
		return -1;  // safety limit
	}
	while (out.size() < nstr) {
		uint32_t len = 0;
		if (!read_u32(data, end, len)) {
			return -1;
		}
		out.push_back(string());
		if (!read_str(data, end, len, out.back())) {
			return -1;
		}
	}
	if (data != end) {
		return -1;  // trailing garbage
	}
	return 0;
}

// executes a parsed command against the in-memory store
// cmd holds the tokens: e.g. {"set", "name", "alex"}
// out is filled with the status code and any returned data
static void do_request(vector<string>& cmd, Response& out) {
	// GET key return val or resnx if not here
	if (cmd.size() == 2 && cmd[0] == "get") {
		auto it = g_data.find(cmd[1]);
		if (it == g_data.end()) {
			out.status = RES_NX;    // key not found
			return;
		}
		const std::string& val = it->second;
		// copy the stored string into the response payload byte-for-byte
		out.data.assign(val.begin(), val.end());
	}
	// SET key value -> store the valuestatus stays RES_OK by default
	else if (cmd.size() == 3 && cmd[0] == "set") {
		// swap steals cmd[2]'s buffer instead of copying it
		g_data[cmd[1]].swap(cmd[2]);
	}
	// DEL key remove the key erase is noop if not there
	else if (cmd.size() == 2 && cmd[0] == "del") {
		g_data.erase(cmd[1]);
	}
	// unknown command or wrong argument count
	else {
		out.status = RES_ERR;
	}
}

// serializes a Response into the wire format: [total_len][status][data...]
// total_len covers the 4-byte status plus the data, so the client knows
// how many bytes to read for the whole reply
static void make_response(const Response& resp, std::vector<uint8_t>& out) {
	uint32_t resp_len = 4 + (uint32_t)resp.data.size();     // 4 = size of status field
	buf_append(out, (const uint8_t*)&resp_len, 4);          // length prefix
	buf_append(out, (const uint8_t*)&resp.status, 4);       // status code
	buf_append(out, resp.data.data(), resp.data.size());    // payload
}

// tries to pull ONE complete message out of the incoming buffer and handle it
// returns true if a message was processed (caller loops to drain more),
// false if we need to wait for more bytes or are closing the connection
static bool try_one_request(Conn* conn) {
	// need at least the 4-byte length header before we can do anything
	if (conn->incoming.size() < 4) {
		return false;   // want read header
	}
	// read the  length from the front of the buffer
	uint32_t len = 0;
	memcpy(&len, conn->incoming.data(), 4);
	if (len > k_max_msg) {
		msg("too long");
		conn->want_close = true;    // protocol violation, drop the client
		return false;
	}
	// the full body hasn't arrived yet, wait for the next read
	if (4 + len > conn->incoming.size()) {
		return false;   // want read body
	}
	// point past the length header to the actual request bytes
	const uint8_t* request = &conn->incoming[4];

	// split the raw bytes into command tokens
	std::vector<std::string> cmd;
	if (parse_req(request, len, cmd) < 0) {
		msg("bad request");
		conn->want_close = true;    // malformed frame, drop the client
		return false;
	}

	// run the command and append its serialized reply to the outgoing buffer
	Response resp;
	do_request(cmd, resp);
	make_response(resp, conn->outgoing);

	// discard the message we just consumed (header + body) from the input buffer
	buf_consume(conn->incoming, 4 + len);
	return true;
}




static void handle_write(Conn* conn) {
	assert(conn->outgoing.size() > 0);

	ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
	if (rv < 0 && errno == EAGAIN) {
		return;  // socket send buffer full, try again later
	}
	if (rv < 0) {
		conn->want_close = true;  // real write error
		return;
	}

	// remove the bytes we managed to send
	buf_consume(conn->outgoing, (size_t)rv);

	// i everything is flushed, switch back to reading
	if (conn->outgoing.size() == 0) {
		conn->want_read = true;
		conn->want_write = false;
	}
}

static void handle_read(Conn* conn) {
	uint8_t buf[64 * 1024];
	ssize_t rv = read(conn->fd, buf, sizeof(buf));

	if (rv < 0 && errno == EAGAIN) {
		return;  // os buffer drained, nothing left to read right now
	}
	if (rv < 0) {
		msg_errno("read() error");
		conn->want_close = true;
		return;
	}
	if (rv == 0) {
		if (conn->incoming.size() == 0) {
			msg("client closed");
		}
		else {
			msg("unexpected EOF");
		}
		conn->want_close = true;
		return;
	}

	// add read bytes to input buffer
	buf_append(conn->incoming, buf, (size_t)rv);

	// drain all the complete messages that arrved
	while (try_one_request(conn)) {}

	// if produced, try to write 
	if (conn->outgoing.size() > 0) {
		conn->want_read = false;
		conn->want_write = true;
		return handle_write(conn);  // optimistic flush
	}
}

////read exactly n bytes from the socket
////this solves partial reads, where tcp splits packets
//
//static int32_t read_full(int fd, char* buf, size_t n) {
//	while (n > 0) {
//		ssize_t rv = read(fd, buf, n);
//		if (rv <= 0) {
//			return -1;//conn error or eof
//		}
//
//		assert(rv <= n); // assert that the size of rcv
//		//is less then what we want to read
//
//		n -= rv; //subract what we got in the current recv
//
//		buf += rv; // change the start of the buffer pointer
//	}
//
//	return 0;
//
//}
//
////writes exactly n bytes to the socket
////kernel write buffer can be full so this solves partial writes
//static int32_t write_all(int fd, const char* buf, size_t n) {
//	while (n > 0) {
//		ssize_t rv = write(fd, buf, n);
//		if (rv <= 0) {
//			return -1;//write error
//		}
//		assert(rv <= n);
//
//		n -= rv;
//		buf += rv;
//	}
//
//	return 0;
//}
//process single length-prefixed req
//static int32_t one_request(int connfd) {
//	char rbuf[4 + k_max_msg];
//
//	int32_t err = read_full(connfd, rbuf, 4);
//	if (err) {
//		//if error and errno is 0 then eof(client close)
//		if (errno != 0) {
//			cerr << "read error" << endl;
//		}
//		return err;
//	}
//	//extract length from payload
//	uint32_t len = 0;
//	memcpy(&len, rbuf, 4);// littleendian
//	if (len > k_max_msg) {
//		cerr << "msg payload too long" << endl;
//		return -1;
//	}
//
//	err = read_full(connfd, rbuf + 4, len);
//	if (err) {
//		cerr << "read body error" << endl;
//		return err;
//	}
//
//	cout << "Client says :" << string(rbuf + 4, len) << endl;
//
//	const char reply[] = "world";
//
//	char wbuf[4 + sizeof(reply)];
//
//	len = strlen(reply);
//	memcpy(wbuf, &len, 4);
//	memcpy(wbuf + 4, reply, len);
//
//	return write_all(connfd, wbuf, 4 + len);
//
//
//}

//static void doSomething(int connfd) {
//	char rbuf[64] = {};
//	ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);//recv
//	if (n < 0) {
//		cerr << "Read error" << endl;
//	}
//	else {
//		cout << "Client says" << rbuf << endl;
//	}
//	const char* reply = "world";
//	write(connfd, reply, strlen(reply));//send
//}



int main() {
	
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		die("socket()");
	}

	//allows use without TIME_WAIT
	int val = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1234);
	addr.sin_addr.s_addr = htonl(0);
	if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) < 0) {
		die("bind()");
	}

	//make listenedr nonblocking
	fd_set_nb(fd);
	if (listen(fd, SOMAXCONN) < 0) {
		die("listen()");
	}

	//map: fd -> Conn* inex from file descr to connection
	vector<Conn*> fd2conn;
	vector<struct pollfd> poll_args;


	while (true) {
		poll_args.clear();

		// slot 0 is always the listening socket
		struct pollfd pfd = { fd, POLLIN, 0 };
		poll_args.push_back(pfd);

		// add every active connection with the events it currently wants
		for (Conn* conn : fd2conn) {
			if (!conn) {
				continue;
			}
			struct pollfd cpfd = { conn->fd, POLLERR, 0 };
			if (conn->want_read)  cpfd.events |= POLLIN;
			if (conn->want_write) cpfd.events |= POLLOUT;
			poll_args.push_back(cpfd);
		}

		// block until at least one socket is ready.
		int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
		if (rv < 0 && errno == EINTR) {
			continue;  // interrupted by a signal, just retry
		}
		if (rv < 0) {
			die("poll");
		}

		// accept new connections on the listener
		// if revents is non zero that means 
		// that the kernel changed it after poll
		// clients are waiting
		if (poll_args[0].revents) {
			if (Conn* conn = handle_accept(fd)) {
				if (fd2conn.size() <= (size_t)conn->fd) {
					fd2conn.resize(conn->fd + 1);
				}
				assert(!fd2conn[conn->fd]);
				fd2conn[conn->fd] = conn;
			}
		}

		//service every ready connection
		for (size_t i = 1; i < poll_args.size(); ++i) {
			uint32_t ready = poll_args[i].revents;
			//no io activity
			//no flag set by the kernel
			if (ready == 0) {
				continue;
			}
			Conn* conn = fd2conn[poll_args[i].fd];
			if (ready & POLLIN) {
				assert(conn->want_read);
				handle_read(conn);
			}
			if (ready & POLLOUT) {
				assert(conn->want_write);
				handle_write(conn);
			}
			if ((ready & POLLERR) || conn->want_close) {
				(void)close(conn->fd);
				fd2conn[conn->fd] = NULL;
				delete conn;
			}
		}
	}

	return 0;
}