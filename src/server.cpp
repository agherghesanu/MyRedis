#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>


using namespace std;

//print error message
static void die(const char* msg) {
	perror(msg);
	exit(EXIT_FAILURE);
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

		doSomething(connfd);
		close(connfd);
	}

	close(fd);
	return 0;


}