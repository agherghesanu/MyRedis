#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cassert>


using namespace std;

const size_t k_max_msg = 4096;

//print error message
static void die(const char* msg) {
	perror(msg);
	exit(EXIT_FAILURE);
}

//read exactly n bytes from the socket
//this solves partial reads, where tcp splits packets

static int32_t read_full(int fd, char* buf, size_t n) {
	while (n > 0) {
		ssize_t rv = read(fd, buf, n);
		if (rv <= 0) {
			return -1;//conn error or eof
		}

		assert(rv <= n); // assert that the size of rcv
		//is less then what we want to read

		n -= rv; //subract what we got in the current recv

		buf += rv; // change the start of the buffer pointer
	}

	return 0;

}

//writes exactly n bytes to the socket
//kernel write buffer can be full so this solves partial writes
static int32_t write_all(int fd, const char* buf, size_t n) {
	while (n > 0) {
		ssize_t rv = write(fd, buf, n);
		if (rv <= 0) {
			return -1;//write error
		}
		assert(rv <= n);

		n -= rv;
		buf += rv;
	}

	return 0;
}
//process single length-prefixed req
static int32_t one_request(int connfd) {
	char rbuf[4 + k_max_msg];

	int32_t err = read_full(connfd, rbuf, 4);
	if (err) {
		//if error and errno is 0 then eof(client close)
		if (errno != 0) {
			cerr << "read error" << endl;
		}
		return err;
	}
	//extract length from payload
	uint32_t len = 0;
	memcpy(&len, rbuf, 4);// littleendian
	if (len > k_max_msg) {
		cerr << "msg payload too long" << endl;
		return -1;
	}

	err = read_full(connfd, rbuf + 4, len);
	if (err) {
		cerr << "read body error" << endl;
		return err;
	}

	cout << "Client says :" << string(rbuf + 4, len) << endl;

	const char reply[] = "world";

	char wbuf[4 + sizeof(reply)];

	len = strlen(reply);
	memcpy(wbuf, &len, 4);
	memcpy(wbuf + 4, reply, len);

	return write_all(connfd, wbuf, 4 + len);


}

static void doSomething(int connfd) {
	char rbuf[64] = {};
	ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);//recv
	if (n < 0) {
		cerr << "Read error" << endl;
	}
	else {
		cout << "Client says" << rbuf << endl;
	}
	const char* reply = "world";
	write(connfd, reply, strlen(reply));//send
}

int main() {
	int fd = socket(AF_INET, SOCK_STREAM, 0);//ip tcp
	if (fd < 0) {
		die("socket");
	}

	int value = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1234);//port
	addr.sin_addr.s_addr = htonl(0);// 0.0.0.0

	int rv = bind(fd, (const struct sockaddr*)&addr, sizeof(addr));
	if (rv < 0) {
		die("bind");
	}

	rv = listen(fd, SOMAXCONN);

	if (rv) {
		die("listen");
	}

	while (true) {
		struct sockaddr_in client_addr = {};
		socklen_t addrlen = sizeof(client_addr);

		int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
		if (connfd < 0) {
			continue;// error
		}

		while (true) {
			int32_t err = one_request(connfd);
			if (err) {
				break;
			}
		}
		cout << "client disconnected" << endl;

		close(connfd);
	}

	close(fd);
	return 0;


}