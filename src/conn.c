#include "ircd.h"

static void origin_recv();
static void toplev_sync();

void u_conn_init(conn)
struct u_conn *conn;
{
	conn->flags = 0;
	conn->sock = NULL;
	u_linebuf_init(&conn->ibuf);
	conn->obuf = malloc(U_CONN_OBUFSIZE);
	conn->obuflen = 0;
	conn->obufsize = U_CONN_OBUFSIZE;
	conn->event = NULL;
	conn->priv = NULL;
	conn->pass = NULL;
	conn->ctx = CTX_UNREG;
}

void u_conn_cleanup(conn)
struct u_conn *conn;
{
	free(conn->obuf);
}

/* sadfaec */
void u_conn_obufsize(conn, obufsize)
struct u_conn *conn;
int obufsize;
{
	char *buf;

	if (conn->obufsize == obufsize)
		return;

	if (conn->obuflen > obufsize)
		conn->obuflen = obufsize;

	buf = malloc(obufsize);
	memcpy(buf, conn->obuf, conn->obuflen);
	free(conn->obuf);
	conn->obuf = buf;
	conn->obufsize = obufsize;
}

void u_conn_out_clear(conn)
struct u_conn *conn;
{
	char *s;

	s = (char*)memchr(conn->obuf, '\r', conn->obuflen);
	if (!s || *++s != '\n')
		s = conn->obuf;
	else
		s++;

	conn->obuflen = s - conn->obuf;
}

void f_str(p, end, s)
char **p, *end, *s;
{
	while (*p < end && *s)
		*(*p)++ = *s++;
}

/* Some day I might write my own string formatter with my own special
   formatting things for use in IRC... but today is not that day */
void u_conn_vf(conn, fmt, va)
struct u_conn *conn;
char *fmt;
va_list va;
{
	char buf[4096];
	char *p, *end;

	p = conn->obuf + conn->obuflen;
	end = conn->obuf + conn->obufsize - 2; /* -2 for \r\n */

	vsprintf(buf, fmt, va);
	u_log(LG_DEBUG, "[%p] <- %s", conn, buf);

	f_str(&p, end, buf);

	*p++ = '\r';
	*p++ = '\n';

	conn->obuflen = p - conn->obuf;

	if (conn->obuflen == conn->obufsize) {
		u_conn_event(conn, EV_SENDQ_FULL);
		u_conn_close(conn);
	}
}

#ifdef STDARG
void u_conn_f(struct u_conn *conn, char *fmt, ...)
#else
void u_conn_f(conn, fmt, va_alist)
struct u_conn *conn;
char *fmt;
va_dcl
#endif
{
	va_list va;
	u_va_start(va, fmt);
	u_conn_vf(conn, fmt, va);
	va_end(va);
}

void u_conn_event(conn, ev)
struct u_conn *conn;
int ev;
{
	u_log(LG_DEBUG, "CONN:EV: [%p] EV=%d", conn, ev);
	if (!conn->event)
		return;
	conn->event(conn, ev);
}

void u_conn_close(conn)
struct u_conn *conn;
{
	conn->flags |= U_CONN_CLOSING;
}

struct u_conn_origin *u_conn_origin_create(io, addr, port)
struct u_io *io;
u_long addr;
u_short port;
{
	struct sockaddr_in sa;
	struct u_conn_origin *orig;
	int fd;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		goto out;

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = addr;

	if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
		goto out;

	if (listen(fd, 5) < 0)
		goto out;

	/* TODO: setsockopt */

	if (!(orig = malloc(sizeof(*orig))))
		goto out_close;

	if (!(orig->sock = u_io_add_fd(io, fd)))
		goto out_free;

	orig->sock->recv = origin_recv;
	orig->sock->send = NULL;
	orig->sock->priv = orig;

	return orig;

out_free:
	free(orig);
out_close:
	close(fd);
out:
	return NULL;
}

static void origin_recv(sock)
struct u_io_fd *sock;
{
	struct u_conn_origin *orig = sock->priv;
	struct u_io_fd *iofd;
	struct u_conn *conn;
	struct sockaddr_in addr;
	int addrlen = sizeof(addr);
	int fd;

	if ((fd = accept(sock->fd, (struct sockaddr*)&addr, &addrlen)) < 0) {
		perror("origin_recv");
		return;
	}

	if (!(iofd = u_io_add_fd(sock->io, fd)))
		return; /* XXX */

	iofd->recv = NULL;
	iofd->send = NULL;

	conn = malloc(sizeof(*conn));
	u_conn_init(conn);
	conn->sock = iofd;
	iofd->priv = conn;

	inet_ntop(AF_INET, &addr.sin_addr, conn->ip, INET_ADDRSTRLEN);

	toplev_sync(iofd);

	u_log(LG_VERBOSE, "Connection from %s", conn->ip);
}

static void toplev_cleanup(iofd)
struct u_io_fd *iofd;
{
	struct u_conn *conn = iofd->priv;
	u_conn_event(conn, EV_DESTROYING);
	u_conn_cleanup(conn);
	close(iofd->fd);
	free(conn);
}

static void toplev_recv(iofd)
struct u_io_fd *iofd;
{
	struct u_conn *conn = iofd->priv;
	char buf[1024];
	int sz;
	struct u_msg msg;

	sz = recv(iofd->fd, buf, 1024-conn->ibuf.pos, 0);

	if (sz <= 0) {
		u_conn_event(conn, sz == 0 ? EV_END_OF_STREAM : EV_RECV_ERROR);
		u_conn_close(conn);
		toplev_sync(iofd);
		return;
	}

	if (u_linebuf_data(&conn->ibuf, buf, sz) < 0) {
		u_conn_event(conn, EV_RECVQ_FULL);
		u_conn_close(conn);
		toplev_sync(iofd);
		return;
	}

	while ((sz = u_linebuf_line(&conn->ibuf, buf, 1024)) != 0) {
		if (sz > 0)
			buf[sz] = '\0';
		if (sz < 0 || strlen(buf) != sz) {
			u_conn_event(conn, EV_RECV_ERROR);
			u_conn_close(conn);
			break;
		}
		u_log(LG_DEBUG, "[%p] -> %s", conn, buf);
		u_msg_parse(&msg, buf);
		u_cmd_invoke(conn, &msg);
	}

	toplev_sync(iofd);
}

static void toplev_send(iofd)
struct u_io_fd *iofd;
{
	struct u_conn *conn = iofd->priv;
	int sz;

	sz = send(iofd->fd, conn->obuf, conn->obuflen, 0);

	if (sz < 0) {
		u_conn_event(conn, EV_SEND_ERROR);
		u_conn_close(conn);
		conn->obuflen = 0;
		toplev_sync(iofd);
		return;
	}

	if (sz > 0) {
		u_memmove(conn->obuf, conn->obuf + sz, conn->obufsize - sz);
		conn->obuflen -= sz;
	}

	toplev_sync(iofd);
}

static void toplev_sync(iofd)
struct u_io_fd *iofd;
{
	struct u_conn *conn = iofd->priv;
	iofd->recv = iofd->send = NULL;
	if (!(conn->flags & U_CONN_CLOSING))
		iofd->recv = toplev_recv;
	if (conn->obuflen > 0)
		iofd->send = toplev_send;
	if (!iofd->recv && !iofd->send)
		toplev_cleanup(iofd);
}
