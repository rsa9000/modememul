/**
 * AT interface
 *
 * Copyright (c) 2022-2023, Sergey Ryazanov <ryazanov.s.a@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "atport.h"

struct atport {
	struct {		/* State flags */
		int echo:1;		/* Echo input or not */
		int echo_junk:1;	/* Echo input junk as well */
	} f;
	enum {			/* AT command parser state */
		AT_PARSER_WAIT_A,
		AT_PARSER_WAIT_T,
		AT_PARSER_WAIT_TERM,
	} pstate;
	struct {		/* Various symbols */
		char s3;
	} sym;
	char cmdbuf[0x200];
	int cmdlen;
	const struct atops *ops;
	void *ops_priv;
	const struct atcmd *cmds;
	void *cmd_priv;
};

static int atport_gen_cmd_e0(struct atport *port)
{
	port->f.echo = 0;

	return 0;
}

static int atport_gen_cmd_e1(struct atport *port)
{
	port->f.echo = 1;

	return 0;
}

static int atport_gen_cmd_s3_read(struct atport *port)
{
	return atport_printf(port, "%03d", port->sym.s3);
}

static int atport_gen_cmd_stub(struct atport *port)
{
	return 0;
}

typedef int (* cmd_a0_t)(void *priv);

static const struct atcmd atport_gen_cmds[] = {
	{"S3", .exec = (cmd_a0_t)atport_gen_cmd_stub,
	       .read = (cmd_a0_t)atport_gen_cmd_s3_read},
	{"E0", .exec = (cmd_a0_t)atport_gen_cmd_e0},
	{"E1", .exec = (cmd_a0_t)atport_gen_cmd_e1},
	{"", .exec = (cmd_a0_t)atport_gen_cmd_stub},
	{NULL}
};

static int atport_cmd_lookup_and_exec(struct atport *port, const char *str,
				      const struct atcmd *cmds, void *priv)
{
	size_t cplen = strcspn(str, "=?");	/* Command prefix length */
	const struct atcmd *c;
	int l;

	for (c = cmds; c->name; ++c) {
		l = strlen(c->name);
		if (l == cplen && strncasecmp(c->name, str, l) == 0)
			break;
	}
	if (!c->name)
		return -ENOENT;

	str += cplen;
	if (strcmp(str, "=?") == 0)		/* AT<cmd>=? */
		return c->test ? c->test(priv) : -ENOENT;
	else if (strcmp(str, "?") == 0)		/* AT<cmd>? */
		return c->read ? c->read(priv) : -ENOENT;
	else if (str[0] == '\0')		/* AT<cmd> */
		return c->exec ? c->exec(priv) : -ENOENT;

	/* AT<cmd>=<param> */
	return c->write ? c->write(str + 1, priv) : -ENOENT;
}

int atport_generic_cmd(struct atport *port, const char *str)
{
	return atport_cmd_lookup_and_exec(port, str, atport_gen_cmds, port);
}

static int atport_custom_cmd(struct atport *port, const char *str)
{
	return atport_cmd_lookup_and_exec(port, str, port->cmds,
					  port->cmd_priv);
}

static int atport_cmd_report_status(struct atport *port, int res)
{
	char buf[0x10], *p = buf, *e = buf + sizeof(buf);

	if (res == 0) {
		/* E3372 prints empty line before each "OK" */
		p += snprintf(p, e - p, "\r\n");
		p += snprintf(p, e - p, "OK");
	} else {
		p += snprintf(p, e - p, "ERROR");
	}
	p += snprintf(p, e - p, "\r\n");

	return port->ops->write(buf, p - buf, port->ops_priv);
}

static int atport_cmd_exec(struct atport *port)
{
	int res;

	res = atport_puts(port, "");
	if (res < 0)
		return res;

	if (port->cmdlen > sizeof(port->cmdbuf))
		return atport_cmd_report_status(port, -EINVAL);

	res = atport_custom_cmd(port, port->cmdbuf);
	if (res == -ENOENT)
		res = atport_generic_cmd(port, port->cmdbuf);

	return atport_cmd_report_status(port, res);
}

/**
 * Implements a minimalistic AT commands parser that echo input back and try to
 * execute it via registedred handlers or return ERROR.
 * See AT command protocol details in the ITU-T V.250 recomendations document.
 *
 * Be aware that this processor is not yet fully V.250 compliant.
 */
int atport_parse(struct atport *port, const char *buf, size_t len)
{
	size_t i, n, s;
	int res;

	for (i = 0, s = 0; i < len; ++i) {
		char c = buf[i];

		if (port->pstate == AT_PARSER_WAIT_A) {
			if (c == 'A' || c == 'a')
				port->pstate = AT_PARSER_WAIT_T;
			else if (!port->f.echo_junk)
				s++;		/* Consume junk symbol */
		} else if (port->pstate == AT_PARSER_WAIT_T) {
			if (c == 'T' || c == 't') {
				port->pstate = AT_PARSER_WAIT_TERM;
			} else {
				port->pstate = AT_PARSER_WAIT_A;
				i--;		/* Check this symbol again */
			}
		} else if (port->pstate == AT_PARSER_WAIT_TERM) {
			if (port->cmdlen < sizeof(port->cmdbuf))
				port->cmdbuf[port->cmdlen] = c;
			port->cmdlen++;

			if (c != port->sym.s3)
				continue;

			n = port->cmdlen < sizeof(port->cmdbuf) ?
			    port->cmdlen : sizeof(port->cmdbuf);
			port->cmdbuf[n - 1] = '\0';

			/* Echo final command part before execution */
			if (port->f.echo && i > s) {
				res = port->ops->write(&buf[s], i - s,
						       port->ops_priv);
				if (res < 0)
					return res;
			}
			s = i + 1;	/* Mark echoed chars */
			res = atport_cmd_exec(port);
			if (res < 0)
				return res;
			port->pstate = AT_PARSER_WAIT_A;
			port->cmdlen = 0;	/* Reset command buffer */
		}
	}

	if (port->f.echo && i > s) {
		/* Echo the processed portion of a not yet completed command */
		res = port->ops->write(&buf[s], i - s,
				       port->ops_priv);
		if (res < 0)
			return res;
	}

	return 0;
}

int atport_puts(struct atport *port, const char *str)
{
	size_t l = strlen(str);
	int res = port->ops->write(str, l, port->ops_priv);

	return res ? res : port->ops->write("\r\n", 2, port->ops_priv);
}

int atport_printf(struct atport *port, const char *fmt, ...)
{
	char buf[0x100];
	va_list ap;
	int res;

	va_start(ap, fmt);
	res = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (res < 0)
		return -errno;

	res = port->ops->write(buf, res < sizeof(buf) ? res : sizeof(buf),
			       port->ops_priv);

	return res ? res : port->ops->write("\r\n", 2, port->ops_priv);
}

struct atport *atport_alloc(const struct atops *ops, void *ops_priv,
			    const struct atcmd *commands, void *cmd_priv)
{
	struct atport *port = calloc(1, sizeof(*port));

	if (!port) {
		fprintf(stderr, "unable to allocate port state\n");
		return NULL;
	}

	port->f.echo = 1;		/* Enable echo by default */
	port->sym.s3 = '\r';		/* Carriage return (see V.250 6.2.1) */
	port->pstate = AT_PARSER_WAIT_A;
	port->ops = ops;
	port->ops_priv = ops_priv;
	port->cmds = commands;
	port->cmd_priv = cmd_priv;

	return port;
}

void atport_free(struct atport *port)
{
	free(port);
}
