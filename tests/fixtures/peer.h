#ifndef TEST_PEER_H
#define TEST_PEER_H

#include "wire.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

struct peer { int port; pid_t pid; };

/* A real TCP peer checks the request, fragments the first header byte by
 * byte, splits the first payload, then coalesces the remaining frames. */
static struct peer start_peer(const unsigned char *data, size_t size,
	const char *request_fixture, int fragmented)
{
	struct sockaddr_in addr;
	socklen_t addr_size = sizeof(addr);
	struct peer peer;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	assert(listener >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001U);
	assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(listener, 1) == 0);
	assert(getsockname(listener, (struct sockaddr *)&addr, &addr_size) == 0);
	peer.port = ntohs(addr.sin_port);
	peer.pid = fork();
	assert(peer.pid >= 0);
	if (peer.pid == 0) {
		unsigned char expected[64], request[29];
		size_t offset = 0;
		int fd;
		alarm(5);
		signal(SIGPIPE, SIG_IGN);
		fd = accept(listener, NULL, NULL);
		assert(fd >= 0);
		close(listener);
		assert(fixture(request_fixture, expected) == sizeof(request));
		while (offset < sizeof(request)) {
			ssize_t n = recv(fd, request + offset, sizeof(request) - offset, 0);
			assert(n > 0);
			offset += (size_t)n;
		}
		assert(memcmp(request, expected, sizeof(request)) == 0);
		offset = 0;
		while (offset < size) {
			size_t chunk = fragmented && offset < 35 ? 1 : size - offset;
			ssize_t n = send(fd, data + offset, chunk, 0);
			if (n < 0 && (errno == EPIPE || errno == ECONNRESET))
				break;
			assert(n > 0);
			offset += (size_t)n;
			if (fragmented && offset < 35) {
				struct timespec delay = {0, 1000000};
				nanosleep(&delay, NULL);
			}
		}
		close(fd);
		_exit(0);
	}
	close(listener);
	return peer;
}

static void finish_peer(struct peer peer)
{
	int status;
	assert(waitpid(peer.pid, &status, 0) == peer.pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#endif
