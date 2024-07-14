#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

//#include <iqdb/socket.h>
#include <iqdb/imgdb.h>
#include <iqdb/debug.h>

namespace iqdb {
// Attach rd/wr FILE to fd and automatically close when going out of scope.
struct socket_stream {
	socket_stream(int sock) :
	  	socket(sock),
		rd(fdopen(sock, "r")),
		wr(fdopen(sock, "w")) {

	  	if (sock == -1 || !rd || !wr) {
			close();
			throw fatal_error("Cannot fdopen socket.");
		}
	}
	~socket_stream() { close(); }
	void close() {
		if (rd) fclose(rd);
		rd=NULL;
		if (wr) fclose(wr);
		wr=NULL;
		if (socket != -1) ::close(socket);
		socket=-1;
	}

	int socket;
	FILE* rd;
	FILE* wr;
};

void die(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
	exit(EXIT_FAILURE);
}

bool set_socket(int fd, struct sockaddr_in& bindaddr, int force) {
	if (fd == -1) die("Can't create socket: %s\n", strerror(errno));

	int opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) die("Can't set SO_REUSEADDR: %s\n", strerror(errno));
	if (bind(fd, (struct sockaddr*) &bindaddr, sizeof(bindaddr)) ||
	    listen(fd, 64)) {
		if (force) die("Can't bind/listen: %s\n", strerror(errno));
		INFO("Socket in use, will replace server later.\n");
		return false;
	} else {
		INFO("Listening on port %d.\n", ntohs(bindaddr.sin_port));
		return true;
	}
}

void rebind(int fd, struct sockaddr_in& bindaddr) {
	int retry = 0;
	INFO("Binding to %08x:%d... ", ntohl(bindaddr.sin_addr.s_addr), ntohs(bindaddr.sin_port));
	while (bind(fd, (struct sockaddr*) &bindaddr, sizeof(bindaddr))) {
		if (retry++ > 60) die("Could not bind: %s.\n", strerror(errno));
		WARN("Can't bind yet: %s.\n", strerror(errno));
		sleep(1);
		INFO("%s", "");
	}
	INFO("bind ok.\n");
	if (listen(fd, 64))
		die("Can't listen: %s.\n", strerror(errno));

	INFO("Listening on port %d.\n", ntohs(bindaddr.sin_port));
}

void server(const char* hostport, int numfiles, char** files, bool listen2) {
	int port;
	char dummy;
	char host[1024];

	std::map<in_addr_t, bool> source_addr;
	struct addrinfo hints;
	struct addrinfo* ai;

	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

        int ret = sscanf(hostport, "%1023[^:]:%i%c", host, &port, &dummy);
	if (ret != 2) { strcpy(host, "localhost"); ret = 1 + sscanf(hostport, "%i%c", &port, &dummy); }
	if (ret != 2) die("Can't parse host/port `%s', got %d.\n", hostport, ret);

	int replace = 0;
	while (numfiles > 0) {
		if (!strcmp(files[0], "-r")) {
			replace = 1;
			numfiles--;
			files++;
		} else if (!strncmp(files[0], "-s", 2)) {
			struct sockaddr_in addr;
			if (int ret = getaddrinfo(files[0] + 2, NULL, &hints, &ai)) 
				die("Can't resolve host %s: %s\n", files[0] + 2, gai_strerror(ret));

			memcpy(&addr, ai->ai_addr, std::min<size_t>(sizeof(addr), ai->ai_addrlen));
			INFO("Restricting connections. Allowed from %s\n", inet_ntoa(addr.sin_addr));
			source_addr[addr.sin_addr.s_addr] = true;
			freeaddrinfo(ai);

			numfiles--;
			files++;
		} else {
			break;
		}
	}

	if (int ret = getaddrinfo(host, NULL, &hints, &ai)) die("Can't resolve host %s: %s\n", host, gai_strerror(ret));

	struct sockaddr_in bindaddr_low;
	struct sockaddr_in bindaddr_high;
	memcpy(&bindaddr_low, ai->ai_addr, std::min<size_t>(sizeof(bindaddr_low), ai->ai_addrlen));
	memcpy(&bindaddr_high, ai->ai_addr, std::min<size_t>(sizeof(bindaddr_high), ai->ai_addrlen));
	bindaddr_low.sin_port = htons(port);
	bindaddr_high.sin_port = htons(port - listen2);
	freeaddrinfo(ai);

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) die("Can't ignore SIGPIPE: %s\n", strerror(errno));

	int fd_high = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	int fd_low = listen2 ? socket(PF_INET, SOCK_STREAM, IPPROTO_TCP) : -1;
	int fd_max = listen2 ? std::max(fd_high, fd_low) : fd_high;
	bool success = set_socket(fd_high, bindaddr_high, !replace);
	if (listen2 && set_socket(fd_low, bindaddr_low, !replace) != success)
		die("Only one socket failed to bind, this is weird, aborting!\n");

	dbSpaceAutoMap dbs(numfiles, imgdb::dbSpace::mode_simple, files);

	if (!success) {
		int other_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (other_fd == -1)
			die("Can't create socket: %s.\n", strerror(errno));
		if (connect(other_fd, (struct sockaddr*) &bindaddr_high, sizeof(bindaddr_high)))
			die("Can't connect to old server: %s.\n", strerror(errno));

		socket_stream stream(other_fd);
		WARN("Sending quit command.\n");
		fputs("quit now\n", stream.wr); fflush(stream.wr);

		char buf[1024];
		while (fgets(buf, sizeof(buf), stream.rd))
			INFO(" --> %s", buf);

		if (listen2) rebind(fd_low, bindaddr_low);
		rebind(fd_high, bindaddr_high);
	}

	fd_set read_fds;
	FD_ZERO(&read_fds);

	while (1) {
		FD_SET(fd_high, &read_fds);
		if (listen2) FD_SET(fd_low,  &read_fds);

		int nfds = select(fd_max + 1, &read_fds, NULL, NULL, NULL);
		if (nfds < 1) die("select() failed: %s\n", strerror(errno));

		struct sockaddr_in client;
		socklen_t len = sizeof(client);

		bool is_high = FD_ISSET(fd_high, &read_fds);

		int fd = accept(is_high ? fd_high : fd_low, (struct sockaddr*) &client, &len);
		if (fd == -1) {
			ERROR("accept() failed: %s\n", strerror(errno));
			continue;
		}

		if (!source_addr.empty() && source_addr.find(client.sin_addr.s_addr) == source_addr.end()) {
			ERROR("REFUSED connection from %s:%d\n", inet_ntoa(client.sin_addr), client.sin_port);
			close(fd);
			continue;
		}

		INFO("Accepted %s connection from %s:%d\n", is_high ? "high priority" : "normal", inet_ntoa(client.sin_addr), client.sin_port);

		struct timeval tv = { 5, 0 };	// 5 seconds
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
		    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
			ERROR("Can't set SO_RCVTIMEO/SO_SNDTIMEO: %s\n", strerror(errno));
		}

		socket_stream stream(fd);

		try {
			do_commands(stream.rd, stream.wr, dbs, is_high);

		} catch (const event_t& event) {
			if (event == DO_QUITANDSAVE) return;

		// Unhandled imgdb::base_error means it was fatal or completely unknown.
		} catch (const imgdb::base_error& err) {
			fprintf(stream.wr, "302 %s %s\n", err.type(), err.what());
			fprintf(stderr, "Caught base_error %s: %s\n", err.type(), err.what());
			throw;

		} catch (const std::exception& err) {
			fprintf(stream.wr, "300 Caught unhandled exception!\n");
			fprintf(stderr, "Caught unhandled exception: %s\n", err.what());
			throw;
		}

		INFO("Connection %s:%d closing.\n", inet_ntoa(client.sin_addr), client.sin_port);
	}
}

}