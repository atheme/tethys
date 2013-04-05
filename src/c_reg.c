/* ircd-micro, c_reg.c -- connection registration commands
   Copyright (C) 2013 Alex Iadicicco

   This file is protected under the terms contained
   in the COPYING file in the project root */

#include "ircd.h"

static void err_already(conn, msg) u_conn *conn; u_msg *msg;
{
	u_conn_num(conn, ERR_ALREADYREGISTERED);
}

static void m_pass(conn, msg) u_conn *conn; u_msg *msg;
{
	if (conn->pass != NULL)
		free(conn->pass);
	conn->pass = (char*)malloc(strlen(msg->argv[0])+1);
	strcpy(conn->pass, msg->argv[0]);
}

static void try_reg(conn) u_conn *conn;
{
	u_user *u = conn->priv;

	if (u_user_state(u, 0) != USER_REGISTERING || !u->nick[0]
			|| !u->ident[0] || !u->gecos[0])
		return;

	u_user_welcome(u);
}

static void m_nick(conn, msg) u_conn *conn; u_msg *msg;
{
	u_user_local *ul;
	char buf[MAXNICKLEN+1];

	u_user_make_ureg(conn);
	ul = conn->priv;

	u_strlcpy(buf, msg->argv[0], MAXNICKLEN+1);

	if (!is_valid_nick(buf)) {
		u_conn_num(conn, ERR_ERRONEOUSNICKNAME, buf);
		return;
	}

	if (u_user_by_nick(buf)) {
		u_conn_num(conn, ERR_NICKNAMEINUSE, buf);
		return;
	}

	u_user_set_nick(USER(ul), buf);

	try_reg(conn);
}

static void m_user(conn, msg) u_conn *conn; u_msg *msg;
{
	u_user_local *ul;
	char buf[MAXIDENT+1];

	u_user_make_ureg(conn);
	ul = conn->priv;

	u_strlcpy(buf, msg->argv[0], MAXIDENT+1);
	if (!is_valid_ident(buf)) {
		u_conn_f(conn, "invalid ident");
		return;
	}
	strcpy(USER(ul)->ident, buf);
	u_strlcpy(USER(ul)->gecos, msg->argv[3], MAXGECOS+1);

	try_reg(conn);
}

static int cap_add(u, cap) u_user *u; char *cap;
{
	char *s;

	for (s=cap; *s; s++)
		*s = isupper(*s) ? tolower(*s) : *s;

	if (!strcmp(cap, "multi-prefix"))
		u->flags |= CAP_MULTI_PREFIX;
	else if (!strcmp(cap, "away-notify"))
		u->flags |= CAP_AWAY_NOTIFY;
	else
		return 0;

	return 1;
}

static void m_cap(conn, msg) u_conn *conn; u_msg *msg;
{
	char ackbuf[BUFSIZE];
	char nakbuf[BUFSIZE];
	u_user_local *ul;
	char *s, *p, *q;

	u_user_make_ureg(conn);
	ul = conn->priv;
	u_user_state(USER(ul), USER_CAP_NEGOTIATION);

	ascii_canonize(msg->argv[0]);

	if (!strcmp(msg->argv[0], "LS")) {
		u_conn_f(conn, ":%s CAP * LS :multi-prefix away-notify", me.name);

	} else if (!strcmp(msg->argv[0], "REQ")) {
		if (msg->argc != 2) {
			u_conn_num(conn, ERR_NEEDMOREPARAMS, "CAP");
			return;
		}

		p = msg->argv[1];
		ackbuf[0] = ackbuf[1] = '\0';
		nakbuf[0] = nakbuf[1] = '\0';
		while ((s = cut(&p, " \t")) != NULL) {
			q = ackbuf;
			if (!cap_add(USER(ul), s))
				q = nakbuf;
			u_strlcat(q, " ", BUFSIZE);
			u_strlcat(q, s, BUFSIZE);
		}

		if (ackbuf[1])
			u_conn_f(conn, ":%s CAP * ACK :%s", me.name, ackbuf+1);
		if (nakbuf[1])
			u_conn_f(conn, ":%s CAP * NAK :%s", me.name, nakbuf+1);

	} else if (!strcmp(msg->argv[0], "END")) {
		u_user_state(USER(ul), USER_REGISTERING);

	}

	try_reg(conn);
}

u_cmd c_reg[] = {
	{ "PASS", CTX_UNREG,  m_pass, 1 },
	{ "PASS", CTX_UREG,   m_pass, 1 },
	{ "PASS", CTX_USER,   err_already, 0 },
	{ "NICK", CTX_UNREG,  m_nick, 1 },
	{ "NICK", CTX_UREG,   m_nick, 1 },
	{ "USER", CTX_UNREG,  m_user, 4 },
	{ "USER", CTX_UREG,   m_user, 4 },
	{ "USER", CTX_USER,   err_already, 0 },
	{ "CAP",  CTX_UNREG,  m_cap,  1 },
	{ "CAP",  CTX_UREG,   m_cap,  1 },
	{ "CAP",  CTX_USER,   err_already, 0 },
	{ "" }
};
