/**
 * Main modem state tracking and command execution module. At the moment
 * this module emulates Huawei E3372 interface.
 *
 * Copyright (c) 2022-2023, Sergey Ryazanov <ryazanov.s.a@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "modem.h"
#include "atport.h"

struct modem_state {
	struct atport *atport;
	struct {
		char *iccid;
		char *imsi;
	} sim;
	struct {
		char *plmn;
		char *name;
		int rssi;
	} net;
	struct {
		int state;
		char *pdu;
	} msgs[10];
};

#define ARRAY_SIZE(__a)		(sizeof(__a)/sizeof((__a)[0]))

static int mdm_cmd_cimi_exec(void *priv)
{
	struct modem_state *mstate = priv;

	return atport_puts(mstate->atport, mstate->sim.imsi);
}

static int mdm_cmd_cgmi_exec(void *priv)
{
	struct modem_state *mstate = priv;

	return atport_puts(mstate->atport, "huawei");
}

static int mdm_cmd_cmgd_write(const char *str, void *priv)
{
	struct modem_state *mstate = priv;
	int idx, len;

	if (sscanf(str, "%d%n", &idx, &len) != 1 || str[len] != '\0')
		return -EINVAL;
	if (idx < 0 || idx >= ARRAY_SIZE(mstate->msgs) ||
	    !mstate->msgs[idx].pdu)
		return -EINVAL;

	free(mstate->msgs[idx].pdu);
	mstate->msgs[idx].pdu = NULL;

	return 0;
}

static int mdm_cmd_cmgf_write(const char *str, void *priv)
{
	if (strcmp(str, "0") != 0)	/* Only PDU mode */
		return -EINVAL;

	return 0;
}

static int mdm_cmd_cmgl_write(const char *str, void *priv)
{
	struct modem_state *mstate = priv;
	int i, l, res;

	if (strcmp(str, "4") != 0)	/* Only "ALL" mode */
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mstate->msgs); ++i) {
		if (!mstate->msgs[i].pdu)
			continue;
		l = strlen(mstate->msgs[i].pdu);
		res = atport_printf(mstate->atport, "+CMGL: %d,%d,,%d", i,
				    mstate->msgs[i].state, l / 2);
		if (res)
			return res;
		res = atport_puts(mstate->atport, mstate->msgs[i].pdu);
		if (res)
			return res;
	}

	return 0;
}

static int mdm_cmd_cops_read(void *priv)
{
	struct modem_state *mstate = priv;

	return atport_printf(mstate->atport, "+COPS: 0,2,\"%s\",7",
			     mstate->net.plmn);
}

static int mdm_cmd_cops_write(const char *str, void *priv)
{
	if (strcmp(str, "3,2") != 0)	/* Support only numeric OP conf */
		return -EINVAL;

	return 0;
}

static int mdm_cmd_cpin_read(void *priv)
{
	return atport_puts(((struct modem_state *)priv)->atport,
			   "+CPIN: READY");
}

static int mdm_cmd_csq_exec(void *priv)
{
	struct modem_state *mstate = priv;
	unsigned signal;

	if (mstate->net.rssi == 0)	/* Unknown */
		signal = 99;
	else if (mstate->net.rssi >= -57)
		signal = 28;
	else if (mstate->net.rssi <= -107)
		signal = 3;
	else
		signal = (mstate->net.rssi + 113) / 2;

	return atport_printf(mstate->atport, "+CSQ: %u,99", signal);
}

static int mdm_cmd_iccid_read(void *priv)
{
	char buf[29], *p = buf, *e = buf + sizeof(buf);
	struct modem_state *mstate = priv;

	p += snprintf(p, e - p, "^ICCID: %s", mstate->sim.iccid);
	if (p - buf < 28)	/* Pad ICCID val to 20 symbols */
		p += snprintf(p, e - p, "%.*s", 28 - (p - buf),
			      "FFFFFFFFFFFFFFFFFFFF");

	return atport_puts(mstate->atport, buf);
}

static int mdm_cmd_sysinfoex_exec(void *priv)
{
	struct modem_state *mstate = priv;

	/* Values:
	 *  2 - Service,
	 *  3 - PS+CS,
	 *  0 - non-roaming,
	 *  1 - SIM valid,
	 *  '' - no SIM lock indication,
	 *  6 - sysmode LTE,
	 *  "LTE" - sysmode name,
	 *  101 - system submode LTE,
	 *  "LTE" - submode name
	 */
	return atport_puts(mstate->atport, "^SYSINFOEX:2,3,0,1,,6,\"LTE\",101,\"LTE\"");
}

struct atcmd modem_atcommands[] = {
	{"+CIMI", .exec = mdm_cmd_cimi_exec},
	{"+CGMI", .exec = mdm_cmd_cgmi_exec},
	{"+CMGD", .write = mdm_cmd_cmgd_write},
	{"+CMGF", .write = mdm_cmd_cmgf_write},
	{"+CMGL", .write = mdm_cmd_cmgl_write},
	{"+COPS", .read = mdm_cmd_cops_read, .write = mdm_cmd_cops_write},
	{"+CPIN", .read = mdm_cmd_cpin_read},
	{"+CSQ", .exec = mdm_cmd_csq_exec},
	{"^ICCID", .read = mdm_cmd_iccid_read},
	{"^SYSINFOEX", .exec = mdm_cmd_sysinfoex_exec},
	{NULL}
};

static void modem_add_sms_recv(struct modem_state *mstate, const char *pdu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mstate->msgs); ++i) {
		if (!mstate->msgs[i].pdu)
			break;
	}
	if (i == ARRAY_SIZE(mstate->msgs)) {
		fprintf(stderr, "no free message slot(s), PDU will be dropped\n");
		return;
	}
	mstate->msgs[i].state = 0;	/* Recv unreaded */
	mstate->msgs[i].pdu = strdup(pdu);
}

void modem_add_test_sms(struct modem_state *mstate)
{
	static const char *basehdr = "07819700214365F7"	/* SMSC */
				     "40"		/* TP-MTI, TP-MMS, ... */
				     "0B819710325476F8"	/* TP-OA */
				     "0000"		/* TP-PID, TP-DCS */
				     ;
	static const char *parts[] = {	/* Text parts */
		"986F79B90D4AC3E7F53688FC66BFE5A0799A0E0AB7CB741668FC76CFCB637A99"
		"5E9783C2E4343C3D1FA7DD6750999DA6B340F33219447E83CAE9FABCFD2683E8"
		"E536FC2D07A5DDE334394DAEBBE9A03A1DC40E8BDFF232A84C0791DFECB7BC0C"
		"6A87CFEE3028CC4EC7EB6117A84A0795DDE936284C06B5D3EE741B642FBBD3E1"
		"360B14AFA7E7",
		"40EEF79C2EAF9341657C593E4ED3C3F4F4DB0DAAB3D9E1F6F80D6287C56F797A"
		"0E72A7E769509D0E0AB3D3F17A1A0E2AE341E53068FC6EB7DFE43768FC76CFCB"
		"F17A98EE0211EBE939285CA7974169795D5E0691DFECB71C947683E465B8BC8C"
		"2EBBC965799A0E4ABB41F637BB0EA787E96590BDCC4ED341E5F9BC0C1AA7D9EC"
		"7A1B447EB3DF",
		"E46550B90E32D7CFE9301DE4AEB3D961103C2C4F87E975B90B54C48FCB707AB9"
		"2E07CDD36E3AE83D1E87CBE3301D34AEC3D3E4303D4C07B9DF6E105CFE4E93CB"
		"6E3A0B34AFBBE9A0B41B34AEB3E16150BC9E06BDCDE6F4381D0691CBF3B2BCEE"
		"A683DA6F363B4D0785DDE936284D0695E774103B2C7ECBEB6D17",
	};
	static char buf[0x200];
	char scts[7 * 2 + 1];
	char udh[6 * 2 + 1];
	struct tm *tm;
	time_t now;
	int i, off;

	now = time(NULL);
	tm = localtime(&now);
	memset(scts, '0', sizeof(scts) - 1);
	scts[0x0] += (tm->tm_year % 100) % 10;
	scts[0x1] += (tm->tm_year % 100) / 10;
	scts[0x2] += (tm->tm_mon + 1) % 10;
	scts[0x3] += (tm->tm_mon + 1) / 10;
	scts[0x4] += tm->tm_mday % 10;
	scts[0x5] += tm->tm_mday / 10;
	scts[0x6] += tm->tm_hour % 10;
	scts[0x7] += tm->tm_hour / 10;
	scts[0x8] += tm->tm_min % 10;
	scts[0x9] += tm->tm_min / 10;
	scts[0xa] += tm->tm_sec % 10;
	scts[0xb] += tm->tm_sec / 10;
	i = tm->tm_gmtoff > 0 ? tm->tm_gmtoff : -tm->tm_gmtoff;
	i = i / 60 / 15;	/* Secs into number of quarters of hour */
	if (tm->tm_gmtoff < 0)	/* Rize high bit for negative offset */
		i += 8 * 10;
	snprintf(&scts[0xc], 3, "%1hhx%1hhx", i % 10, i / 10);

	/* Join base header with TP-SCTS */
	off = snprintf(buf, sizeof(buf), "%s%s", basehdr, scts);

	/* Prepare UDH template with SM concatenation element */
	snprintf(udh, sizeof(udh), "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
		 5, 0, 3, (unsigned char)random(), ARRAY_SIZE(parts), 0);

	for (i = 0; i < ARRAY_SIZE(parts); ++i) {
		int l = ((strlen(parts[i]) + 12) / 2 * 8) / 7;	/* Septets */

		snprintf(&udh[5 * 2], 3, "%02hhX", i + 1);
		snprintf(&buf[off], sizeof(buf) - off, "%02hhX%s%s", l, udh,
			 parts[i]);
		modem_add_sms_recv(mstate, buf);
	}
}

void modem_tick(struct modem_state *mstate)
{
	/* Make RSSI more dynamic and increase it each tick */
	mstate->net.rssi += 2;
	if (mstate->net.rssi > -55)
		mstate->net.rssi = -109;
}

void modem_set_atport(struct modem_state *mstate, struct atport *atport)
{
	mstate->atport = atport;
}

struct modem_state *modem_alloc(void)
{
	struct modem_state *mstate = calloc(1, sizeof(*mstate));

	if (!mstate) {
		fprintf(stderr, "unable to allocate the modem state");
		return NULL;
	}

	/* Almost arbitrary codes/values */
	mstate->sim.iccid = "8970169934461058920";
	mstate->sim.imsi = "250692933657186";
	mstate->net.plmn = "25069";
	mstate->net.name = "FunComm";
	mstate->net.rssi = -60;

	return mstate;
}

void modem_free(struct modem_state *mstate)
{
	int i;

	if (!mstate)
		return;

	for (i = 0; i < sizeof(mstate->msgs)/sizeof(mstate->msgs[0]); ++i)
		free(mstate->msgs[i].pdu);

	free(mstate);
}
