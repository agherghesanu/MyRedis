#include <iostream>
#include <vector>
#include <string>
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
#include "conn.h"

using namespace std;

const size_t k_max_msg = 4096;



//print error message
static void die(const char* msg) {
	perror(msg);
	abort();
}

// sets up non blocking file descriptor
static void fd_set_nb(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		die("fcntl error");
	}
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) {
		die("fcnt error");
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

//splits the lengtrh preixed protocl packed down into components
static int32_t parse_req(const uint8_t* data, size_t size, vector<string>& out) {
	const uint8_t* end = data + size;
	uint32_t nstr = 0;
	if (!read_u32(data, end, nstr)) {
		return -1;
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
		return -1;
	}
	return 0;
}

static bool try_one_request(Conn* conn) {
	//4 bytes for length header precond
	if (conn->incoming.size() < 4) {
		return false;
	}

	// read length 
	uint32_t len = 0;
	memcpy(&len, conn->incoming.data(), 4);
	if (len > k_max_msg) {
		cout << "message too long";
		conn->want_close = true;
		return false;
	}

	//check if full message arrived
	if (4 + len > conn->incoming.size()) {
		return false;
	}


	//point aftert length header and parse into cmd
	const uint8_t* request = &conn->incoming[4];
	vector<string> cmd;
	if (parse_req(request, len, cmd) < 0) {
		conn->want_close = true;
		return false;
	}


	cout << "client executed query with string ";
	for (auto& s : cmd) {
		cout << s << " ";
	}
	cout << endl;

	//echo the message back
	buf_append(conn->outgoing, (const uint8_t*)&len, 4);
	buf_append(conn->outgoing, request, len);

	//remove from input buffer
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
		conn->want_close = true;  // real read error
		return;
	}
	if (rv == 0) {
		conn->want_close = true;  // eof probs or closed connection
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
		if (poll_args[0].revents & POLLIN) {
			struct sockaddr_in client_addr = {};
			socklen_t addrlen = sizeof(client_addr);
			int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
			if (connfd >= 0) {
				fd_set_nb(connfd);

				Conn* conn = new Conn();
				conn->fd = connfd;
				conn->want_read = true;

				if (fd2conn.size() <= (size_t)conn->fd) {
					fd2conn.resize(conn->fd + 1, nullptr);
				}
				fd2conn[conn->fd] = conn;

				cout << "Accepted connection at fd: " << connfd << endl;
			}
		}

		//service every ready connection
		for (size_t i = 1; i < poll_args.size(); ++i) {
			uint32_t ready = poll_args[i].revents;
			if (ready == 0) {
				continue;
			}

			Conn* conn = fd2conn[poll_args[i].fd];

			if (ready & POLLIN)  handle_read(conn);
			if (ready & POLLOUT) handle_write(conn);

			// close on error or request
			if ((ready & POLLERR) || conn->want_close) {
				cout << "Closing connection at fd: " << conn->fd << endl;
				close(conn->fd);
				fd2conn[conn->fd] = nullptr;
				delete conn;
			}
		}
	}

	return 0;
}