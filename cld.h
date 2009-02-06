#ifndef __CLD_H__
#define __CLD_H__

/*
 * Copyright 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <sys/epoll.h>
#include <netinet/in.h>
#include <glib.h>
#include "cldb.h"
#include "cld_msg.h"

struct client;
struct server_socket;

#define ALIGN8(n) ((8 - ((n) & 7)) & 7)

enum {
	SFL_FOREGROUND		= (1 << 0),	/* run in foreground */
};

enum server_poll_type {
	spt_udp,				/* UDP socket */
};

struct server_poll {
	enum server_poll_type	poll_type;	/* spt_xxx above */
	union {
		struct server_socket	*sock;
	} u;
};

struct client {
	struct sockaddr_in6	addr;		/* inet address */
	socklen_t		addr_len;	/* inet address len */
	char			addr_host[64];	/* ASCII version of inet addr */
};

struct server_stats {
	unsigned long		poll;		/* number polls */
	unsigned long		event;		/* events dispatched */
	unsigned long		max_evt;	/* epoll events max'd out */
};

struct server_socket {
	int			fd;
	struct server_poll	poll;
	struct epoll_event	evt;
};

struct server {
	unsigned long		flags;		/* SFL_xxx above */

	char			*data_dir;	/* database/log dir */
	char			*pid_file;	/* PID file */

	char			*port;		/* bind port */

	int			epoll_fd;	/* epoll descriptor */

	struct cldb		cldb;		/* database info */

	GList			*sockets;

	struct server_stats	stats;		/* global statistics */
};

/* msg.c */
extern bool msg_new_cli(struct server_socket *sock, DB_TXN *txn,
		 struct client *cli, uint8_t *raw_msg, size_t msg_len);
extern bool msg_open(struct server_socket *sock, DB_TXN *txn,
		 struct client *cli, uint8_t *raw_msg, size_t msg_len);

/* server.c */
extern struct server cld_srv;
extern int debugging;
extern void resp_err(struct server_socket *sock, struct client *cli,
		     struct cld_msg_hdr *msg, enum cle_err_codes errcode);
extern void resp_ok(struct server_socket *sock, struct client *cli,
		    struct cld_msg_hdr *msg);

/* util.c */
extern int write_pid_file(const char *pid_fn);
extern void syslogerr(const char *prefix);
extern int fsetflags(const char *prefix, int fd, int or_flags);

#endif /* __CLD_H__ */
