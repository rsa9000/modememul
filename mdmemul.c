/**
 * Modem AT interface emulator
 *
 * Copyright (c) 2022-2023, Sergey Ryazanov <ryazanov.s.a@gmail.com>
 */

#define _XOPEN_SOURCE		600	/* for posix_pty/grantpt/unlockpt */

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/select.h>

#include "atport.h"
#include "modem.h"

static struct cmn_state {
	struct atport *atport;
	struct modem_state *mdm;
	int pty_fd;
	int sig_usr1;
} __state, *state = &__state;

static void dump_exchange(const char *pref, const char *buf, size_t len)
{
	size_t i;

	printf("%s[%zd]: ", pref, len);
	for (i = 0; i < len; ++i) {
		if (buf[i] == '\r')
			fputs("\\r", stdout);
		else if (buf[i] == '\n')
			fputs("\\n", stdout);
		else
			putc(buf[i], stdout);
	}
	putc('\n', stdout);
}

static int port_write(const char *buf, size_t len, void *priv)
{
	struct cmn_state *state = priv;

	if (len == 0)
		return 0;

	dump_exchange("Tx", buf, len);

	return write(state->pty_fd, buf, len) < 0 ? -errno : 0;
}

struct atops atops = {
	.write = port_write,
};

static int open_pty(const char *linkname)
{
	int fd = posix_openpt(O_RDWR | O_NOCTTY);
	const char *devname;
	struct termios tio;

	if (fd < 0) {
		perror("posix_openpt()");
		return -1;
	}

	/* Disable master echo */
	tcgetattr(fd, &tio);
	tio.c_lflag = ~ECHO;
	tcsetattr(fd, TCSANOW, &tio);

	/* Allow slave openning */
	if (grantpt(fd) < 0 || unlockpt(fd) < 0) {
		perror("grantpt()/unlockpt()");
		goto err_close;
	}

	devname = ptsname(fd);
	if (!devname) {
		perror("ptsname()");
		goto err_close;
	}

	printf("Slave device name - %s\n", devname);

	/* Open slave PTY device to prevent it destroying on client close */
	if (open(devname, O_RDWR | O_NOCTTY) < 0) {
		fprintf(stderr, "unable to open slave PTY: %s\n",
			strerror(errno));
		goto err_close;
	}

	while (linkname) {
		int res;

		res = symlink(devname, linkname);
		if (res == 0)
			break;
		if (errno != EEXIST) {
			perror("symlink()");
			goto err_close;
		}

		if (unlink(linkname)) {
			fprintf(stderr, "unable to remove existing %s file: %s\n",
				linkname, strerror(errno));
			goto err_close;
		}
	}

	return fd;

err_close:
	close(fd);
	return -1;
}

static void sig_usr1(int signal)
{
	/* NB: do not call modem API here to avoid races */
	state->sig_usr1 = 1;
}

static void usage(const char *name)
{
	printf(
		"Modem AT interface emulator\n"
		"\n"
		"Usage:\n"
		"  %s -h\n"
		"  %s -l <filename>\n"
		"\n"
		"Options:\n"
		"  -h        Print this message\n"
		"  -l <filename> Create a symbolic link that points to the actual pseudo\n"
		"            terminal device\n"
		"\n", name, name
	);
}

int main(int argc, char *argv[])
{
	const char *name = basename(argv[0]);
	const char *slinkname = NULL;
	struct timespec nexttime;
	struct sigaction sigact;
	int opt;

	while (1) {
		opt = getopt(argc, argv, "+hl:");
		if (opt == -1)
			break;
		switch (opt) {
		case 'h':
			usage(name);
			return EXIT_SUCCESS;
		case 'l':
			slinkname = optarg;
			break;
		default:
			return EXIT_FAILURE;
		}
	}

	srandom(time(NULL));

	state->pty_fd = open_pty(slinkname);
	if (state->pty_fd < 0)
		return EXIT_FAILURE;

	state->mdm = modem_alloc();
	if (!state->mdm)
		return EXIT_FAILURE;

	state->atport = atport_alloc(&atops, state, modem_atcommands,
				     state->mdm);
	if (!state->atport) {
		close(state->pty_fd);
		return EXIT_FAILURE;
	}
	modem_set_atport(state->mdm, state->atport);

	clock_gettime(CLOCK_MONOTONIC, &nexttime);

	memset(&sigact, 0x00, sizeof(sigact));
	sigact.sa_handler = sig_usr1;
	sigaction(SIGUSR1, &sigact, NULL);

	while (1) {
		struct timeval timeout;
		struct timespec now;
		char buf[0x100];
		fd_set rfds;
		int res;

		/**
		 * To maintain stable frequency by price of phase instabillity,
		 * each time calculate a new time interval to target tick
		 * moment. And just move the target time each time when "timer"
		 * fired.
		 *
		 * NB: interval could be negative in case of missed momment.
		 */
		clock_gettime(CLOCK_MONOTONIC, &now);
		timeout.tv_sec = nexttime.tv_sec - now.tv_sec;
		timeout.tv_usec = (nexttime.tv_nsec - now.tv_nsec + 500) / 1000;
		if (timeout.tv_usec < 0) {
			timeout.tv_sec -= 1;
			timeout.tv_usec += 1000 * 1000;
		}
		if (timeout.tv_sec < 0)
			memset(&timeout, 0x00, sizeof(timeout));

		FD_ZERO(&rfds);
		FD_SET(state->pty_fd, &rfds);
		res = select(state->pty_fd + 1, &rfds, NULL, NULL, &timeout);
		if (res < 0 && errno != EINTR) {
			perror("select()");
			continue;
		}

		if (state->sig_usr1) {
			state->sig_usr1 = 0;
			modem_add_test_sms(state->mdm);
			continue;
		}

		if (res == 0) {	/* Timeout (tick time) */
			modem_tick(state->mdm);
			nexttime.tv_sec += 1;	/* Move next target moment */
			continue;
		}

		if (!FD_ISSET(state->pty_fd, &rfds))
			continue;

		res = read(state->pty_fd, buf, sizeof(buf));
		if (res < 0) {
			if (errno == EINTR)
				continue;
			perror("read()");
			return EXIT_FAILURE;
		}

		dump_exchange("Rx", buf, res);
		if (atport_parse(state->atport, buf, res) < 0)
			break;
	}

	modem_free(state->mdm);
	atport_free(state->atport);
	close(state->pty_fd);

	return EXIT_SUCCESS;
}
