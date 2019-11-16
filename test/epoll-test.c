#define _GNU_SOURCE

#include <atf-c.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static int
fd_pipe(int fds[3])
{
	fds[2] = -1;
	ATF_REQUIRE(pipe2(fds, O_CLOEXEC) >= 0);
	return 0;
}

static int
fd_domain_socket(int fds[3])
{
	fds[2] = -1;
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, fds) >= 0);
	return 0;
}

static int fd_leak_test_a;
static int fd_leak_test_b;
static void
init_fd_checking(void)
{
	/* We check for fd leaks after each test. Remember fd numbers for
	 * checking here. */
	int fds[3];
	fd_pipe(fds);

	fd_leak_test_a = fds[0];
	fd_leak_test_b = fds[1];

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
}
static void
check_for_fd_leaks(void)
{
	/* Test that all fds of previous tests
	 * have been closed successfully. */

	int fds[3];
	fd_pipe(fds);

	ATF_REQUIRE(fds[0] == fd_leak_test_a);
	ATF_REQUIRE(fds[1] == fd_leak_test_b);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
}

#define ATF_TC_BODY_FD_LEAKCHECK(tc, tcptr)                                   \
	static void fd_leakcheck_##tc##_body(atf_tc_t const *tcptr __attribute__((__unused__))); \
	ATF_TC_BODY(tc, tcptr)                                                \
	{                                                                     \
		init_fd_checking();                                           \
		fd_leakcheck_##tc##_body(tcptr);                              \
		check_for_fd_leaks();                                         \
	}                                                                     \
	static void fd_leakcheck_##tc##_body(atf_tc_t const *tcptr __attribute__((__unused__)))

static int connector_epfd = -1;

static void *
connector_client(void *arg)
{
	(void)arg;

	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		warnx("exiting connector_client 1");
		return NULL;
	}

	if (connector_epfd >= 0) {
		int ep = connector_epfd;

		struct epoll_event event;
		event.events = EPOLLOUT | EPOLLIN;
		event.data.fd = sock;

		if (epoll_ctl(ep, EPOLL_CTL_ADD, sock, &event) < 0) {
			warnx("exiting connector_client 2");
			return NULL;
		}

		int ret;

		for (int i = 0; i < 3; ++i) {
			ret = epoll_wait(ep, &event, 1, 300);
			if (ret != 1) {
				warnx("exiting connector_client 3");
				return NULL;
			}

			if (event.events != (EPOLLOUT | EPOLLHUP)) {
				warnx("exiting connector_client 4");
				return NULL;
			}
		}
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return NULL;
	}

	return (void *)(intptr_t)sock;
}

static int
fd_tcp_socket(int fds[3])
{
	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return (-1);
	}

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, /**/
		&enable, sizeof(int)) < 0) {
		return (-1);
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return (-1);
	}

	if (bind(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		err(1, "bind");
		return (-1);
	}

	if (listen(sock, 5) < 0) {
		return (-1);
	}

	pthread_t client_thread;
	if (pthread_create(&client_thread, NULL, connector_client, NULL) < 0) {
		return (-1);
	}

	int conn = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
	if (conn < 0) {
		return (-1);
	}

	void *client_socket = NULL;

	if (pthread_join(client_thread, &client_socket) < 0) {
		return (-1);
	}

	fds[0] = conn;
	fds[1] = (int)(intptr_t)client_socket;
	fds[2] = sock;
	return 0;
}

ATF_TC_WITHOUT_HEAD(epoll__simple);
ATF_TC_BODY_FD_LEAKCHECK(epoll__simple, tc)
{
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE(close(fd) == 0);

	ATF_REQUIRE_ERRNO(EINVAL, epoll_create(0) < 0);

	ATF_REQUIRE(epoll_create(1) >= 0);
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__leakcheck);
ATF_TC_BODY_FD_LEAKCHECK(epoll__leakcheck, tc)
{
	int fd;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);

	atf_tc_expect_fail("Test that the leak check works");
}

ATF_TC_WITHOUT_HEAD(epoll__invalid_op);
ATF_TC_BODY_FD_LEAKCHECK(epoll__invalid_op, tc)
{
	int fd;

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = 0;

	ATF_REQUIRE((fd = epoll_create1(EPOLL_CLOEXEC)) >= 0);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(fd, 3, 5, &event) < 0);
	ATF_REQUIRE_ERRNO(EFAULT, epoll_ctl(fd, 3, 5, NULL) < 0);
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__simple_wait);
ATF_TC_BODY_FD_LEAKCHECK(epoll__simple_wait, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event event;

	ATF_REQUIRE(epoll_wait(ep, &event, 1, 1) == 0);
	ATF_REQUIRE(epoll_wait(ep, &event, 1, 0) == 0);

	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__event_size);
ATF_TC_BODY_FD_LEAKCHECK(epoll__event_size, tc)
{
	struct epoll_event event;
#if defined(__amd64__)
	ATF_REQUIRE(sizeof(event) == 12);
#else
	// TODO(jan): test for other architectures
	abort();
#endif
}

static void
simple_epollin_impl(int (*fd_fun)(int fds[3]))
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_fun(fds);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) >= 0);

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.data.fd == fds[0]);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__simple_epollin);
ATF_TC_BODY_FD_LEAKCHECK(epoll__simple_epollin, tc)
{
	simple_epollin_impl(fd_pipe);
	simple_epollin_impl(fd_domain_socket);
	simple_epollin_impl(fd_tcp_socket);
}

static void *
sleep_then_write(void *arg)
{
	usleep(100000);
	uint8_t data = '\0';
	write((int)(intptr_t)arg, &data, 1);
	return NULL;
}

static void
sleep_argument_impl(int sleep)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) >= 0);

	pthread_t writer_thread;
	ATF_REQUIRE(pthread_create(&writer_thread, NULL, sleep_then_write,
			(void *)(intptr_t)(fds[1])) == 0);

	ATF_REQUIRE(epoll_wait(ep, &event, 1, sleep) == 1);

	ATF_REQUIRE(pthread_join(writer_thread, NULL) == 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__sleep_argument);
ATF_TC_BODY_FD_LEAKCHECK(epoll__sleep_argument, tc)
{
	sleep_argument_impl(-1);
	sleep_argument_impl(-2);
}

ATF_TC_WITHOUT_HEAD(epoll__remove_nonexistent);
ATF_TC_BODY_FD_LEAKCHECK(epoll__remove_nonexistent, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	ATF_REQUIRE_ERRNO(ENOENT,
	    epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL) < 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__add_remove);
ATF_TC_BODY_FD_LEAKCHECK(epoll__add_remove, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL) == 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

static void
add_existing_impl(bool change_udata)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.u32 = 42;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	if (change_udata) {
		event.data.u32 = 43;
	}

	ATF_REQUIRE_ERRNO(EEXIST,
	    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0);
	ATF_REQUIRE_ERRNO(EEXIST,
	    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) < 0);

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.data.u32 == 42);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__add_existing);
ATF_TC_BODY_FD_LEAKCHECK(epoll__add_existing, tc)
{
	add_existing_impl(true);
	add_existing_impl(false);
}

ATF_TC_WITHOUT_HEAD(epoll__modify_existing);
ATF_TC_BODY_FD_LEAKCHECK(epoll__modify_existing, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	event.events = 0;
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0);

	uint8_t data = '\0';
	write(fds[1], &data, 1);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, 50) == 0);

	event.events = EPOLLIN;
	event.data.fd = 42;
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0);

	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);
	ATF_REQUIRE(event_result.data.fd == 42);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__modify_nonexisting);
ATF_TC_BODY_FD_LEAKCHECK(epoll__modify_nonexisting, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = fds[0];

	ATF_REQUIRE_ERRNO(ENOENT,
	    epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) < 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__poll_only_fd);
ATF_TC_BODY_FD_LEAKCHECK(epoll__poll_only_fd, tc)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fd = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		// Don't fail the test when there is no sound card.
		goto out;
	}

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fd;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, 300) == 1);

	ATF_REQUIRE(event_result.events == EPOLLOUT);
	ATF_REQUIRE(event_result.data.fd == fd);

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == 0);

	ATF_REQUIRE_ERRNO(ENOENT, /**/
	    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) < 0);

	ATF_REQUIRE(close(fd) == 0);
out:
	ATF_REQUIRE(close(ep) == 0);
}

static void
no_epollin_on_closed_empty_pipe_impl(bool do_write_data)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	uint8_t data = '\0';
	if (do_write_data) {
		write(fds[1], &data, 1);
	}
	close(fds[1]);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.events ==
	    (EPOLLHUP | (do_write_data ? EPOLLIN : 0)));

	ATF_REQUIRE(read(fds[0], &data, 1) >= 0);

	ATF_REQUIRE(event_result.data.fd == fds[0]);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__no_epollin_on_closed_empty_pipe);
ATF_TC_BODY_FD_LEAKCHECK(epoll__no_epollin_on_closed_empty_pipe, tcptr)
{
	no_epollin_on_closed_empty_pipe_impl(false);
	no_epollin_on_closed_empty_pipe_impl(true);
}

ATF_TC_WITHOUT_HEAD(epoll__write_to_pipe_until_full);
ATF_TC_BODY_FD_LEAKCHECK(epoll__write_to_pipe_until_full, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == 0);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.data.fd == fds[1]);
	ATF_REQUIRE(event_result.events == EPOLLOUT);

	uint8_t data[512] = {0};

	for (int i = 0; i < 128; ++i) {
		write(fds[1], &data, sizeof(data));
	}

	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, 300) == 0);

	event.events = EPOLLIN;
	event.data.fd = fds[0];
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.data.fd == fds[0]);
	ATF_REQUIRE(event_result.events == EPOLLIN);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__realtime_timer);
ATF_TC_BODY_FD_LEAKCHECK(epoll__realtime_timer, tcptr)
{
	struct itimerspec new_value;
	struct timespec now;
	uint64_t exp, tot_exp;
	ssize_t s;

	ATF_REQUIRE(clock_gettime(CLOCK_REALTIME, &now) == 0);

	new_value.it_value.tv_sec = now.tv_sec + 1;
	new_value.it_value.tv_nsec = now.tv_nsec;
	new_value.it_interval.tv_sec = 0;
	new_value.it_interval.tv_nsec = 100000000;

	int fd = timerfd_create(CLOCK_REALTIME, 0);
	ATF_REQUIRE(fd >= 0);

	ATF_REQUIRE(timerfd_settime(fd, /**/
			TFD_TIMER_ABSTIME, &new_value, NULL) == 0);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = fd;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);

	struct epoll_event event_result;

	for (tot_exp = 0; tot_exp < 3;) {
		ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

		ATF_REQUIRE(event_result.events == EPOLLIN);
		ATF_REQUIRE(event_result.data.fd == fd);

		s = read(fd, &exp, sizeof(uint64_t));
		ATF_REQUIRE(s == sizeof(uint64_t));

		tot_exp += exp;
		printf("read: %llu; total=%llu\n", (unsigned long long)exp,
		    (unsigned long long)tot_exp);
	}

	ATF_REQUIRE(close(ep) == 0);
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__simple_signalfd);
ATF_TC_BODY_FD_LEAKCHECK(epoll__simple_signalfd, tcptr)
{
	sigset_t mask;
	int sfd;
	struct signalfd_siginfo fdsi;
	ssize_t s;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

	sfd = signalfd(-1, &mask, 0);
	ATF_REQUIRE(sfd >= 0);

	kill(getpid(), SIGINT);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT;
	event.data.fd = sfd;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &event) == 0);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.events == EPOLLIN);
	ATF_REQUIRE(event_result.data.fd == sfd);

	s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
	ATF_REQUIRE(s == sizeof(struct signalfd_siginfo));

	ATF_REQUIRE(fdsi.ssi_signo == SIGINT);

	ATF_REQUIRE(close(ep) == 0);
	ATF_REQUIRE(close(sfd) == 0);
}

static void
socket_shutdown_impl(bool specify_rdhup)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_tcp_socket(fds);

	uint32_t rdhup_flag = specify_rdhup ? EPOLLRDHUP : 0;

	struct epoll_event event;
	event.events = EPOLLOUT | EPOLLIN | (specify_rdhup ? 0 : EPOLLRDHUP);
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	event.events = EPOLLOUT | EPOLLIN | rdhup_flag;
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0);

	shutdown(fds[1], SHUT_WR);

	for (;;) {
		ATF_REQUIRE(epoll_wait(ep, &event, 1, -1) == 1);

		fprintf(stderr, "got event: %x\n", (int)event.events);

		if (event.events == EPOLLOUT) {
			/* skip spurious event generated by EVFILT_WRITE */
			/* TODO(jan): find a better solution */
			continue;
		}

		if (event.events == (EPOLLOUT | EPOLLIN | rdhup_flag)) {
			uint8_t buf;
			ssize_t ret = read(fds[0], &buf, 1);
			ATF_REQUIRE(ret == 0);

			shutdown(fds[0], SHUT_RDWR);
		} else if (event.events ==
		    (EPOLLOUT | EPOLLIN | rdhup_flag | EPOLLHUP)) {
			/* close() may fail here! Don't check return code. */
			close(fds[0]);
			break;
		} else {
			ATF_REQUIRE(false);
		}
	}

	ATF_REQUIRE(epoll_wait(ep, &event, 1, 300) == 0);

	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__socket_shutdown);
ATF_TC_BODY_FD_LEAKCHECK(epoll__socket_shutdown, tcptr)
{
	socket_shutdown_impl(true);
	socket_shutdown_impl(false);
}

ATF_TC_WITHOUT_HEAD(epoll__epollhup_on_fresh_socket);
ATF_TC_BODY_FD_LEAKCHECK(epoll__epollhup_on_fresh_socket, tcptr)
{
	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	ATF_REQUIRE(sock >= 0);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = sock;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, sock, &event) == 0);

	int ret;

	for (int i = 0; i < 3; ++i) {
		ret = epoll_wait(ep, &event, 1, 100);
		ATF_REQUIRE(ret == 1);

		ATF_REQUIRE(event.events == EPOLLHUP);

		usleep(100000);
	}

	ATF_REQUIRE(close(ep) == 0);
	ATF_REQUIRE(close(sock) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__timeout_on_listening_socket);
ATF_TC_BODY_FD_LEAKCHECK(epoll__timeout_on_listening_socket, tcptr)
{
	int sock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	ATF_REQUIRE(sock >= 0);

	int enable = 1;
	ATF_REQUIRE(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, /**/
			&enable, sizeof(int)) == 0);

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	ATF_REQUIRE(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

	ATF_REQUIRE(bind(sock, /**/
			(struct sockaddr const *)&addr, sizeof(addr)) == 0);

	ATF_REQUIRE(listen(sock, 5) == 0);

	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = sock;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, sock, &event) == 0);

	int ret;

	for (int i = 0; i < 3; ++i) {
		ret = epoll_wait(ep, &event, 1, 100);
		ATF_REQUIRE(ret == 0);

		usleep(100000);
	}

	ATF_REQUIRE(close(ep) == 0);
	ATF_REQUIRE(close(sock) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__epollerr_on_closed_pipe);
ATF_TC_BODY_FD_LEAKCHECK(epoll__epollerr_on_closed_pipe, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[1];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == 0);

	for (;;) {
		struct epoll_event event_result;
		ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

		ATF_REQUIRE(event_result.data.fd == fds[1]);

		if (event_result.events == EPOLLOUT) {
			// continue
		} else if (event_result.events == (EPOLLOUT | EPOLLERR)) {
			break;
		} else {
			ATF_REQUIRE(false);
		}

		uint8_t data[512] = {0};
		write(fds[1], &data, sizeof(data));

		ATF_REQUIRE(close(fds[0]) == 0);
	}

	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

static void
shutdown_behavior_impl(int (*fd_fun)(int fds[3]))
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	connector_epfd = ep;

	int fds[3];
	fd_fun(fds);

	connector_epfd = -1;

	int counter = 0;
	char c = 42;
	write(fds[0], &c, 1);

	struct epoll_event event;
	event.events = EPOLLOUT | EPOLLIN;
	event.data.fd = fds[1];
	epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event);

	errno = 0;

	for (;;) {
		struct epoll_event event_result;
		int n;
		ATF_REQUIRE((n = epoll_wait(ep, &event_result, 1, -1)) == 1);

		ATF_REQUIRE(event_result.data.fd == fds[1]);

		// fprintf(stderr, "got event: %x %d\n",
		// (int)event_result.events,
		//     (int)event_result.events);

		if (event_result.events & EPOLLIN) {
			ATF_REQUIRE((n = (int)read(fds[1], &c, 1)) == 1);

			++counter;

			if (counter <= 5) {
				write(fds[0], &c, 1);
			} else if (counter == 6) {
				write(fds[0], &c, 1);
				shutdown(fds[0], SHUT_WR);
				usleep(100000);
			} else {
				uint8_t data[512] = {0};
				write(fds[1], &data, sizeof(data));

				close(fds[0]);

				event.events = EPOLLOUT;
				event.data.fd = fds[1];
				ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, /**/
						fds[1], &event) == 0);

				usleep(100000);
			}

		} else if (event_result.events == EPOLLOUT) {
			write(event.data.fd, &c, 1);
			// continue
		} else if (fd_fun == fd_domain_socket &&
		    (event_result.events & (EPOLLOUT | EPOLLHUP)) ==
			(EPOLLOUT | EPOLLHUP)) {
			// TODO(jan): Linux sets EPOLLERR in addition
			{
				int error = 0;
				socklen_t errlen = sizeof(error);
				getsockopt(fds[1], SOL_SOCKET, SO_ERROR,
				    (void *)&error, &errlen);
				fprintf(stderr, "socket error: %d (%s)\n",
				    error, strerror(error));
			}
			break;
		} else if (fd_fun == fd_tcp_socket &&
		    event_result.events == (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
			{
				int error = 0;
				socklen_t errlen = sizeof(error);
				getsockopt(fds[1], SOL_SOCKET, SO_ERROR,
				    (void *)&error, &errlen);
				fprintf(stderr, "socket error: %d (%s)\n",
				    error, strerror(error));
			}
			break;
		} else {
			ATF_REQUIRE(false);
		}
	}

	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__shutdown_behavior);
ATF_TC_BODY_FD_LEAKCHECK(epoll__shutdown_behavior, tcptr)
{
	shutdown_behavior_impl(fd_tcp_socket);
	shutdown_behavior_impl(fd_domain_socket);
}

static void *
datagram_connector(void *arg)
{
	(void)arg;

	int sock = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		return NULL;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return NULL;
	}

	if (connect(sock, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
		return NULL;
	}

	fprintf(stderr, "got client\n");

	uint8_t data = '\0';
	write(sock, &data, 0);
	usleep(500000);
	close(sock);

	return NULL;
}

ATF_TC_WITHOUT_HEAD(epoll__datagram_connection);
ATF_TC_BODY_FD_LEAKCHECK(epoll__datagram_connection, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int sock = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	ATF_REQUIRE(sock >= 0);

	int enable = 1;
	ATF_REQUIRE(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, /**/
			&enable, sizeof(int)) == 0);

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	ATF_REQUIRE(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

	ATF_REQUIRE(bind(sock, /**/
			(struct sockaddr const *)&addr, sizeof(addr)) == 0);

	pthread_t client_thread;
	pthread_create(&client_thread, NULL, datagram_connector, NULL);

	int fds[2];
	fds[0] = sock;

	struct epoll_event event;
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	fprintf(stderr, "got event: %x %d\n", (int)event_result.events,
	    (int)event_result.events);

	ATF_REQUIRE(event_result.events == EPOLLIN);

	uint8_t data = '\0';
	ATF_REQUIRE(read(fds[0], &data, 1) >= 0);

	ATF_REQUIRE(event_result.data.fd == fds[0]);

	pthread_join(client_thread, NULL);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__epollout_on_own_shutdown);
ATF_TC_BODY_FD_LEAKCHECK(epoll__epollout_on_own_shutdown, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_tcp_socket(fds);

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = fds[0];

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	shutdown(fds[0], SHUT_WR);
	usleep(100000);

	struct epoll_event event_result;
	ATF_REQUIRE(epoll_wait(ep, &event_result, 1, -1) == 1);

	ATF_REQUIRE(event_result.data.fd == fds[0]);

	fprintf(stderr, "got events: %x\n", (unsigned)event_result.events);

	ATF_REQUIRE(event_result.events == EPOLLOUT);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(fds[2] == -1 || close(fds[2]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__recursive_register);
ATF_TC_BODY_FD_LEAKCHECK(epoll__recursive_register, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int ep_inner = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep_inner >= 0);

	struct epoll_event event;
	event.events = EPOLLOUT;
	event.data.fd = ep_inner;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, ep_inner, &event) == 0);
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_DEL, ep_inner, NULL) == 0);

	ATF_REQUIRE(close(ep_inner) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__remove_closed);
ATF_TC_BODY_FD_LEAKCHECK(epoll__remove_closed, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event = {0};
	event.events = EPOLLIN;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);

	// Trying to delete an event that was already deleted by closing the
	// associated fd should fail.
	ATF_REQUIRE_ERRNO(EBADF,
	    epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], &event) < 0);

	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__add_different_file_with_same_fd_value);
ATF_TC_BODY_FD_LEAKCHECK(epoll__add_different_file_with_same_fd_value, tcptr)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);

	int fds[3];
	fd_pipe(fds);

	struct epoll_event event = {0};
	event.events = EPOLLIN;

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);

	// Note: This wouldn't be needed under Linux as the close() calls above
	// properly removes the descriptor from the epoll instance. However, in
	// our epoll emulation we cannot (yet?) reliably detect if a descriptor
	// has been closed before it is deleted from the epoll instance.
	// See also: https://github.com/jiixyj/epoll-shim/pull/7
	ATF_REQUIRE_ERRNO(EBADF,
	    epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], &event) < 0);

	// Creating new pipe. The file descriptors will have the same numerical
	// values as the previous ones.
	fd_pipe(fds);

	// If status of closed fds would not be cleared, adding an event with
	// the fd that has the same numerical value as the closed one would
	// fail.
	int ret;
	struct epoll_event event2 = {0};
	event2.events = EPOLLIN;
	ATF_REQUIRE((ret = epoll_ctl(ep, /**/
			 EPOLL_CTL_ADD, fds[0], &event2)) == 0);

	pthread_t writer_thread;
	pthread_create(&writer_thread, NULL, sleep_then_write,
	    (void *)(intptr_t)(fds[1]));

	ATF_REQUIRE((ret = epoll_wait(ep, &event, 1, 300)) == 1);

	pthread_join(writer_thread, NULL);

	ATF_REQUIRE(close(fds[0]) == 0);
	ATF_REQUIRE(close(fds[1]) == 0);
	ATF_REQUIRE(close(ep) == 0);
}

ATF_TC_WITHOUT_HEAD(epoll__invalid_writes);
ATF_TC_BODY_FD_LEAKCHECK(epoll__invalid_writes, tcptr)
{
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

	char dummy = 0;

	{
		fd = signalfd(-1, &mask, 0);
		ATF_REQUIRE(fd >= 0);
		ATF_REQUIRE_ERRNO(EINVAL, write(fd, &dummy, 1) < 0);
		ATF_REQUIRE(close(fd) == 0);
	}

	{
		fd = timerfd_create(CLOCK_MONOTONIC, 0);
		ATF_REQUIRE(fd >= 0);
		ATF_REQUIRE_ERRNO(EINVAL, write(fd, &dummy, 1) < 0);
		ATF_REQUIRE(close(fd) == 0);
	}

	{
		fd = epoll_create1(EPOLL_CLOEXEC);
		ATF_REQUIRE(fd >= 0);
		ATF_REQUIRE_ERRNO(EINVAL, write(fd, &dummy, 1) < 0);
		ATF_REQUIRE_ERRNO(EINVAL, read(fd, &dummy, 1) < 0);
		ATF_REQUIRE(close(fd) == 0);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, epoll__simple);
	ATF_TP_ADD_TC(tp, epoll__leakcheck);
	ATF_TP_ADD_TC(tp, epoll__invalid_op);
	ATF_TP_ADD_TC(tp, epoll__simple_wait);
	ATF_TP_ADD_TC(tp, epoll__event_size);
	ATF_TP_ADD_TC(tp, epoll__simple_epollin);
	ATF_TP_ADD_TC(tp, epoll__sleep_argument);
	ATF_TP_ADD_TC(tp, epoll__remove_nonexistent);
	ATF_TP_ADD_TC(tp, epoll__add_remove);
	ATF_TP_ADD_TC(tp, epoll__add_existing);
	ATF_TP_ADD_TC(tp, epoll__modify_existing);
	ATF_TP_ADD_TC(tp, epoll__modify_nonexisting);
	ATF_TP_ADD_TC(tp, epoll__poll_only_fd);
	ATF_TP_ADD_TC(tp, epoll__no_epollin_on_closed_empty_pipe);
	ATF_TP_ADD_TC(tp, epoll__write_to_pipe_until_full);
	ATF_TP_ADD_TC(tp, epoll__realtime_timer);
	ATF_TP_ADD_TC(tp, epoll__simple_signalfd);
	ATF_TP_ADD_TC(tp, epoll__socket_shutdown);
	ATF_TP_ADD_TC(tp, epoll__epollhup_on_fresh_socket);
	ATF_TP_ADD_TC(tp, epoll__timeout_on_listening_socket);
	ATF_TP_ADD_TC(tp, epoll__epollerr_on_closed_pipe);
	ATF_TP_ADD_TC(tp, epoll__shutdown_behavior);
	ATF_TP_ADD_TC(tp, epoll__datagram_connection);
	ATF_TP_ADD_TC(tp, epoll__epollout_on_own_shutdown);
	ATF_TP_ADD_TC(tp, epoll__recursive_register);
	ATF_TP_ADD_TC(tp, epoll__remove_closed);
	ATF_TP_ADD_TC(tp, epoll__add_different_file_with_same_fd_value);
	ATF_TP_ADD_TC(tp, epoll__invalid_writes);

	return atf_no_error();
}
