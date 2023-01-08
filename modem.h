/**
 * Main modem state tracking and command execution module header file
 *
 * Copyright (c) 2022-2023, Sergey Ryazanov <ryazanov.s.a@gmail.com>
 */

#ifndef _MODEM_H_
#define _MODEM_H_

#include "atport.h"

struct modem_state;

extern struct atcmd modem_atcommands[];

void modem_add_test_sms(struct modem_state *mstate);
void modem_tick(struct modem_state *mstate);
void modem_set_atport(struct modem_state *mstate, struct atport *atport);
struct modem_state *modem_alloc(void);
void modem_free(struct modem_state *mstate);

#endif	/* _MODEM_H_ */
