/**
 * AT interface header file
 *
 * Copyright (c) 2022-2023, Sergey Ryazanov <ryazanov.s.a@gmail.com>
 */

#ifndef _ATPORT_H_
#define _ATPORT_H_

struct atops {
	int (*write)(const char *buf, size_t len, void *priv);
};

struct atcmd {
	const char *name;				/* e.g. "+COPS" */
	int (*exec)(void *priv);			/* AT<cmd> */
	int (*read)(void *priv);			/* AT<cmd>? */
	int (*test)(void *priv);			/* AT<cmd>=? */
	int (*write)(const char *str, void *priv);	/* AT<cmd>=<param> */
};

struct atport;

int atport_parse(struct atport *port, const char *buf, size_t len);
int atport_puts(struct atport *port, const char *str);
int atport_printf(struct atport *port, const char *fmt, ...);
struct atport *atport_alloc(const struct atops *ops, void *ops_priv,
			    const struct atcmd *commands, void *cmd_priv);
void atport_free(struct atport *port);

#endif	/* _ATPORT_H_ */
