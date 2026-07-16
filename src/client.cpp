#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cassert>

using namespace std;

const size_t k_max_msg = 4096;

static void die(const char* msg) {
	perror(msg);
	exit(EXIT_FAILURE);
}

static int32_t read_full(int fd, char* buf, size_t n) {
	while (n > 0) {
		ssize_t rv = read(fd, buf, n);
		if (rv <= 0) {
			return -1;
		}
		assert((size_t)rv <= n);
		n -= (size_t)rv;
		buf += rv;
	}
	return 0;
}

static int32_t write_all(int fd, const char* buf, size_t n) {
	while (n > 0) {
		ssize_t rv = write(fd, buf, n);
		if (rv <= 0) {
			return -1;
		}
		assert((size_t)rv <= n);
		n -= (size_t)rv;
		buf += rv;
	}
	return 0;
}

static int32_t query(int fd, const char* text) {
	uint32_t len = strlen(text);
	if (len > k_max_msg) {
		return -1;
	}

	char wbuf[4 + k_max_msg];
	memcpy(wbuf, &len, 4);
	memcpy(wbuf + 4, text, len);

	int32_t err = write_all(fd, wbuf, 4 + len);

	if (err) {
		return err;
	}

	char rbuf[4 + k_max_msg];
	err = read_full(fd, rbuf, 4);

	if (err) {
		cerr << "read header error" << endl; 
	}

	memcpy(&len, rbuf, 4);

	if (len > k_max_msg) {
		cerr << "msg too long" << endl;
		return - 1;
	}

	err = read_full(fd, rbuf + 4, len);
	if (err) {
		cerr << "read body error" << endl;
		return err;
	}

	cout << "server says" << string(rbuf + 4, len) << endl;
	return 0;
}

int main() {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		die("socket");
	}

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = ntohs(1234);
	addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);

	int rv = connect(fd, (const struct sockaddr*)&addr, sizeof(addr));
	if (rv) {
		die("connect");
	}

	int32_t err = query(fd, "hello1");
	if (err) {
		close(fd);
		return 0;
	}

	err = query(fd, "hello2");
	if (err) {
		close(fd);
		return 0;
	}

	err = query(fd, "hello3");
	if (err) {
		close(fd);
		return 0;
	}





	
}