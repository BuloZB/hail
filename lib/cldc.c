
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

#define _GNU_SOURCE
#include "cld-config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <glib.h>
#include <cldc.h>

enum {
	CLDC_MSG_EXPIRE		= 5 * 60,
	CLDC_MSG_SCAN		= 60,
	CLDC_MSG_RETRY		= 5,
	CLDC_MSG_REMEMBER	= 25,
	CLDC_SESS_EXPIRE	= 2 * 60,
	CLDC_MAX_DATA_SZ	= 256 * 1024,
	CLDC_MAX_DATA_PKT_SZ	= 1024,
	CLDC_MAX_DATA_PKTS	= (CLDC_MAX_DATA_SZ / CLDC_MAX_DATA_PKT_SZ) + 2,
};

static bool authsign(struct cldc_session *, struct cld_packet *, size_t);

static const struct cld_msg_hdr def_msg_ack = {
	.magic		= CLD_MSG_MAGIC,
	.op		= cmo_ack,
};

#ifndef HAVE_STRNLEN
static size_t strnlen(const char *s, size_t maxlen)
{
	int len = 0;

	if (!s)
		return 0;

	while ((len < maxlen) && (*s)) {
		s++;
		len++;
	}

	return len;
}
#endif
#ifndef EBADRQC
#define EBADRQC 56
#endif
#ifndef EBADSLT
#define EBADSLT 57
#endif
#ifndef EBADE
#define EBADE 52
#endif

void cldc_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int ack_seqid(struct cldc_session *sess, uint64_t seqid_le)
{
	struct cld_packet *pkt;
	struct cld_msg_hdr *resp;
	size_t pkt_len;

	pkt_len = sizeof(*pkt) + sizeof(*resp) + SHA_DIGEST_LENGTH;
	pkt = alloca(pkt_len);
	memset(pkt, 0, pkt_len);

	memcpy(pkt->magic, CLD_PKT_MAGIC, CLD_MAGIC_SZ);
	pkt->seqid = seqid_le;
	memcpy(pkt->sid, sess->sid, CLD_SID_SZ);
	pkt->n_msg = 1;
	strncpy(pkt->user, sess->user, CLD_MAX_USERNAME - 1);

	resp = (struct cld_msg_hdr *) (pkt + 1);
	memcpy(resp, &def_msg_ack, sizeof(*resp));

	if (!authsign(sess, pkt, pkt_len)) {
		sess->act_log("authsign failed 2\n");
		return -1;
	}

	return sess->ops->pkt_send(sess->private, sess->addr, sess->addr_len,
				   pkt, pkt_len);
}

static int cldc_rx_generic(struct cldc_session *sess,
			   const struct cld_packet *pkt,
			   const void *msgbuf,
			   size_t buflen)
{
	const struct cld_msg_resp *resp = msgbuf;
	struct cldc_msg *req = NULL;
	ssize_t rc;
	GList *tmp;

	if (buflen < sizeof(*resp))
		return -1008;

	tmp = sess->out_msg;
	while (tmp) {
		req = tmp->data;

		if (sess->verbose)
			sess->act_log("rx_gen: comparing req->xid (%llu) with resp->xid_in (%llu)\n",
			        (unsigned long long)
					GUINT64_FROM_LE(req->xid),
			        (unsigned long long)
					GUINT64_FROM_LE(resp->xid_in));

		if (req->xid == resp->xid_in)
			break;
		tmp = tmp->next;
	}
	if (!tmp)
		return -1005;

	if (sess->verbose)
		sess->act_log("rx_gen: issuing completion and acking\n");

	if (!req->done) {
		req->done = true;

		if (req->cb) {
			rc = req->cb(req, msgbuf, buflen, true);
			if (rc < 0)
				return rc;
		}
	}

	return ack_seqid(sess, pkt->seqid);
}

static int cldc_rx_data_c(struct cldc_session *sess,
			   const struct cld_packet *pkt,
			   const void *msgbuf,
			   size_t buflen)
{
	const struct cld_msg_data *data = msgbuf;
	struct cldc_stream *str = NULL;
	uint32_t seg, seg_len;
	GList *tmp;
	const void *p;

	if (buflen < sizeof(*data))
		return -1008;

	seg = GUINT32_FROM_LE(data->seg);
	seg_len = GUINT32_FROM_LE(data->seg_len);

	if (buflen < (sizeof(*data) + seg_len))
		return -1008;

	/* look for stream w/ our strid */
	tmp = sess->streams;
	while (tmp) {
		str = tmp->data;
		if (str->strid_le == data->strid)
			break;
		tmp = tmp->next;
	}

	/* if not found, return */
	if (!tmp)
		return -1009;

	/* verify segment number is what we expect */
	if (seg != str->next_seg) {
		if (sess->verbose)
			sess->act_log("rx_data: seg mismatch %u expect %u\n",
				      seg, str->next_seg);
		return -1010;
	}

	if (seg_len > str->size_left) {
		if (sess->verbose)
			sess->act_log("rx_data: verflow seg_len %u left %u\n",
				      seg_len, str->size_left);
		return -1011;
	}

	p = data;
	p += sizeof(*data);
	memcpy(str->bufp, p, seg_len);

	str->bufp += seg_len;
	str->size_left -= seg_len;
	str->next_seg++;

	/* if terminator, process completion */
	if (seg_len == 0 && str->copts.cb) {
		if (str->size_left)
			sess->act_log("rx_data: size %u short by %u\n",
				      str->size, str->size_left);
		str->copts.u.get.buf = str->buf;
		str->copts.u.get.size = str->size - str->size_left;
		str->copts.cb(&str->copts, CLE_OK);
		sess->streams = g_list_delete_link(sess->streams, tmp);
		memset(str, 0, sizeof(*str));
		free(str);
	}

	return 0;
}

static int cldc_rx_event(struct cldc_session *sess,
			 const struct cld_packet *pkt,
			 const void *msgbuf,
			 size_t buflen)
{
	const struct cld_msg_event *ev = msgbuf;
	struct cldc_fh *fh = NULL;
	int i;

	if (buflen < sizeof(*ev))
		return -1008;

	for (i = 0; i < sess->fh->len; i++) {
		fh = &g_array_index(sess->fh, struct cldc_fh, i);
		if (fh->fh_le == ev->fh)
			break;
		else
			fh = NULL;
	}

	if (!fh)
		return -1011;

	sess->ops->event(sess->private, sess, fh,
			 GUINT32_FROM_LE(ev->events));

	return 0;
}

static int cldc_rx_not_master(struct cldc_session *sess,
			      const struct cld_packet *pkt,
			      const void *msgbuf,
			      size_t buflen)
{
	sess->act_log("FIXME: not-master message received\n");
	return -1055;	/* FIXME */
}

static void sess_expire_outmsg(struct cldc_session *sess, time_t current_time)
{
	GList *tmp, *tmp1;

	tmp = sess->out_msg;
	while (tmp) {
		struct cldc_msg *msg;

		tmp1 = tmp;
		tmp = tmp->next;

		msg = tmp1->data;
		if (current_time > msg->expire_time) {
			free(msg);
			sess->out_msg = g_list_delete_link(sess->out_msg, tmp1);
		}
	}

	sess->msg_scan_time = current_time + CLDC_MSG_SCAN;
}

static const char *user_key(struct cldc_session *sess, const char *user)
{
	if (strcmp(sess->user, user))
		return NULL;

	return sess->secret_key;
}

static bool authcheck(struct cldc_session *sess, const struct cld_packet *pkt,
		      size_t buflen)
{
	size_t userlen = strnlen(pkt->user, sizeof(pkt->user));
	const char *key;
	unsigned char md[SHA_DIGEST_LENGTH];
	unsigned int md_len = 0;
	const void *buf = pkt;

	/* forbid zero-len and max-len (no nul) usernames */
	if (userlen < 1 || userlen >= sizeof(pkt->user))
		return false;

	key = user_key(sess, pkt->user);
	if (!key)
		return false;

	HMAC(EVP_sha1(), key, strlen(key), buf, buflen - SHA_DIGEST_LENGTH,
	     md, &md_len);

	if (md_len != SHA_DIGEST_LENGTH)
		sess->act_log("authsign BUG: md_len != SHA_DIGEST_LENGTH\n");

	if (memcmp(buf + buflen - SHA_DIGEST_LENGTH, md, SHA_DIGEST_LENGTH))
		return false;

	return true;
}

static bool authsign(struct cldc_session *sess, struct cld_packet *pkt,
		     size_t buflen)
{
	const char *key;
	unsigned char md[SHA_DIGEST_LENGTH];
	unsigned int md_len = 0;
	void *buf = pkt;

	key = user_key(sess, pkt->user);
	if (!key)
		return false;

	HMAC(EVP_sha1(), key, strlen(key), buf, buflen - SHA_DIGEST_LENGTH,
	     md, &md_len);

	if (md_len != SHA_DIGEST_LENGTH)
		sess->act_log("authsign BUG: md_len != SHA_DIGEST_LENGTH\n");

	memcpy(buf + (buflen - SHA_DIGEST_LENGTH), md, SHA_DIGEST_LENGTH);

	return true;
}

static const char *opstr(enum cld_msg_ops op)
{
	switch (op) {
	case cmo_nop:		return "cmo_nop";
	case cmo_new_sess:	return "cmo_new_sess";
	case cmo_open:		return "cmo_open";
	case cmo_get_meta:	return "cmo_get_meta";
	case cmo_get:		return "cmo_get";
	case cmo_data_s:	return "cmo_data_s";
	case cmo_put:		return "cmo_put";
	case cmo_close:		return "cmo_close";
	case cmo_del:		return "cmo_del";
	case cmo_lock:		return "cmo_lock";
	case cmo_unlock:	return "cmo_unlock";
	case cmo_trylock:	return "cmo_trylock";
	case cmo_ack:		return "cmo_ack";
	case cmo_end_sess:	return "cmo_end_sess";
	case cmo_ping:		return "cmo_ping";
	case cmo_not_master:	return "cmo_not_master";
	case cmo_event:		return "cmo_event";
	case cmo_data_c:	return "cmo_data_c";
	default:		return "(unknown)";
	}
}

static int cldc_receive_msg(struct cldc_session *sess,
			    const struct cld_packet *pkt,
			    size_t pkt_len,
			    const struct cld_msg_hdr *msg,
			    size_t msglen)
{
	switch(msg->op) {
	case cmo_nop:
	case cmo_close:
	case cmo_del:
	case cmo_lock:
	case cmo_unlock:
	case cmo_trylock:
	case cmo_put:
	case cmo_new_sess:
	case cmo_end_sess:
	case cmo_open:
	case cmo_data_s:
	case cmo_get_meta:
	case cmo_get:
		return cldc_rx_generic(sess, pkt, msg, msglen);
	case cmo_not_master:
		return cldc_rx_not_master(sess, pkt, msg, msglen);
	case cmo_event:
		return cldc_rx_event(sess, pkt, msg, msglen);
	case cmo_data_c:
		return cldc_rx_data_c(sess, pkt, msg, msglen);
	case cmo_ping:
		return ack_seqid(sess, pkt->seqid);
	case cmo_ack:
		return -EBADRQC;
	}

	/* unknown op code */
	return -EBADRQC;
}

int cldc_receive_pkt(struct cldc_session *sess,
		     const void *net_addr, size_t net_addrlen,
		     const void *pktbuf, size_t pkt_len)
{
	const struct cld_packet *pkt = pktbuf;
	const struct cld_msg_hdr *msg = (struct cld_msg_hdr *) (pkt + 1);
	size_t msglen = pkt_len - sizeof(*pkt);
	struct timeval tv;
	time_t current_time;
	uint64_t seqid;

	gettimeofday(&tv, NULL);
	current_time = tv.tv_sec;

	if (pkt_len < (sizeof(*pkt) + sizeof(*msg) + SHA_DIGEST_LENGTH)) {
		if (sess->verbose)
			sess->act_log("receive_pkt: msg too short\n");
		return -EPROTO;
	}

	if (sess->verbose)
		sess->act_log("receive pkt: len %u, "
			"op %s, seqid %llu, user %s\n",
			(unsigned int) pkt_len,
			opstr(msg->op),
			(unsigned long long) GUINT64_FROM_LE(pkt->seqid),
			pkt->user);

	if (memcmp(pkt->magic, CLD_PKT_MAGIC, sizeof(pkt->magic))) {
		if (sess->verbose)
			sess->act_log("receive_pkt: bad pkt magic\n");
		return -EPROTO;
	}
	if (memcmp(msg->magic, CLD_MSG_MAGIC, sizeof(msg->magic))) {
		if (sess->verbose)
			sess->act_log("receive_pkt: bad msg magic\n");
		return -EPROTO;
	}

	/* check HMAC signature */
	if (!authcheck(sess, pkt, pkt_len)) {
		if (sess->verbose)
			sess->act_log("receive_pkt: invalid auth\n");
		return -EACCES;
	}

	/* verify stored server addr matches pkt addr */
	if (((sess->addr_len != net_addrlen) ||
	    memcmp(sess->addr, net_addr, net_addrlen))) {
		if (sess->verbose)
			sess->act_log("receive_pkt: server address mismatch\n");
		return -EBADE;
	}

	/* expire old sess outgoing messages */
	if (current_time >= sess->msg_scan_time)
		sess_expire_outmsg(sess, current_time);

	/* verify (or set, for new-sess) sequence id */
	seqid = GUINT64_FROM_LE(pkt->seqid);
	if (msg->op == cmo_new_sess) {
		sess->next_seqid_in = seqid + 1;
		sess->next_seqid_in_tr =
			sess->next_seqid_in - CLDC_MSG_REMEMBER;

		if (sess->verbose)
			sess->act_log("receive_pkt: "
					 "setting next_seqid_in to %llu\n",
					 (unsigned long long) seqid);
	} else if (msg->op != cmo_not_master) {
		if (seqid != sess->next_seqid_in) {
			if (seqid_in_range(seqid,
					   sess->next_seqid_in_tr,
					   sess->next_seqid_in))
				return ack_seqid(sess, pkt->seqid);

			if (sess->verbose)
				sess->act_log("receive_pkt: bad seqid %llu\n",
					(unsigned long long) seqid);
			return -EBADSLT;
		}
		sess->next_seqid_in++;
		sess->next_seqid_in_tr++;
	}

	sess->expire_time = current_time + CLDC_SESS_EXPIRE;

	return cldc_receive_msg(sess, pkt, pkt_len, msg, msglen);
}

static void sess_next_seqid(struct cldc_session *sess, uint64_t *seqid)
{
	uint64_t rc = GUINT64_TO_LE(sess->next_seqid_out++);
	*seqid = rc;
}

static struct cldc_msg *cldc_new_msg(struct cldc_session *sess,
				     const struct cldc_call_opts *copts,
				     enum cld_msg_ops op,
				     size_t msg_len)
{
	struct cldc_msg *msg;
	struct cld_msg_hdr *hdr;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	msg = calloc(1, sizeof(*msg) + msg_len + SHA_DIGEST_LENGTH);
	if (!msg)
		return NULL;

	msg->sess = sess;
	msg->expire_time = tv.tv_sec + CLDC_MSG_EXPIRE;

	sess_next_seqid(sess, &msg->seqid);

	__cld_rand64(&msg->xid);

	msg->data_len = msg_len + SHA_DIGEST_LENGTH;
	if (copts)
		memcpy(&msg->copts, copts, sizeof(msg->copts));

	msg->pkt.seqid = msg->seqid;

	hdr = (struct cld_msg_hdr *) &msg->data[0];
	memcpy(&hdr->magic, CLD_MSG_MAGIC, CLD_MAGIC_SZ);
	hdr->op = op;
	hdr->xid = msg->xid;

	return msg;
}

static void sess_msg_drop(struct cldc_session *sess)
{
	GList *tmp = sess->out_msg;
	struct cldc_msg *msg;

	while (tmp) {
		msg = tmp->data;
		tmp = tmp->next;

		if (!msg->done && msg->cb)
			msg->cb(msg, NULL, 0, false);

		free(msg);
	}

	g_list_free(sess->out_msg);
	sess->out_msg = NULL;
}

static void sess_expire(struct cldc_session *sess)
{
	sess->expired = true;
	sess_msg_drop(sess);

	sess->ops->timer_ctl(sess->private, false, NULL, NULL, 0);

	sess->ops->event(sess->private, sess, NULL, CE_SESS_FAILED);
	/* FIXME why not sess_free here */
}

static int sess_timer(struct cldc_session *sess, void *priv)
{
	struct cldc_msg *msg;
	GList *tmp = sess->out_msg;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	if (tv.tv_sec > sess->expire_time) {
		sess_expire(sess);
		return 0;
	}

	while (tmp) {
		msg = tmp->data;
		tmp = tmp->next;

		if (msg->done)
			continue;

		msg->retries++;
		sess->ops->pkt_send(sess->private,
			       sess->addr, sess->addr_len,
			       &msg->pkt,
			       sizeof(msg->pkt) + msg->data_len);
	}

	sess->ops->timer_ctl(sess->private, true, sess_timer, sess,
			     CLDC_MSG_RETRY);
	return CLDC_MSG_RETRY;
}

static int sess_send(struct cldc_session *sess,
		     struct cldc_msg *msg)
{
	struct cld_packet *pkt = &msg->pkt;

	memcpy(pkt->magic, CLD_PKT_MAGIC, CLD_MAGIC_SZ);
	memcpy(pkt->sid, sess->sid, CLD_SID_SZ);
	pkt->n_msg = 1;
	strncpy(pkt->user, sess->user, CLD_MAX_USERNAME - 1);

	/* sign message */
	if (!authsign(sess, pkt, sizeof(*pkt) + msg->data_len))
		return -1;

	/* add to list of outgoing packets, waiting to be ack'd */
	sess->out_msg = g_list_append(sess->out_msg, msg);

	/* attempt first send */
	if (sess->ops->pkt_send(sess->private,
		       sess->addr, sess->addr_len,
		       pkt, sizeof(*pkt) + msg->data_len) < 0)
		return -1;

	return 0;
}

static int sess_stream_open(struct cldc_session *sess,
			     uint64_t strid_le,
			     uint32_t size,
			     const struct cldc_call_opts *copts)
{
	struct cldc_stream *str;

	str = calloc(1, sizeof(*str) + size);
	if (!str)
		return -ENOMEM;

	str->strid_le = strid_le;
	str->size = size;
	str->size_left = size;
	str->bufp = &str->buf[0];
	memcpy(&str->copts, copts, sizeof(*copts));

	sess->streams = g_list_append(sess->streams, str);

	return 0;
}

static void sess_free(struct cldc_session *sess)
{
	GList *tmp;

	if (!sess)
		return;

	if (sess->fh)
		g_array_free(sess->fh, TRUE);

	tmp = sess->out_msg;
	while (tmp) {
		free(tmp->data);
		tmp = tmp->next;
	}
	g_list_free(sess->out_msg);

	tmp = sess->streams;
	while (tmp) {
		free(tmp->data);
		tmp = tmp->next;
	}
	g_list_free(sess->streams);

	memset(sess, 0, sizeof(*sess));
	free(sess);
}

static ssize_t end_sess_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, bool ok)
{
	const struct cld_msg_resp *resp = resp_p;
	enum cle_err_codes resp_rc = CLE_OK;

	if (!ok)
		resp_rc = CLE_TIMEOUT;
	else
		resp_rc = GUINT32_FROM_LE(resp->code);

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	sess_free(msg->sess);
	return 0;
}

int cldc_end_sess(struct cldc_session *sess, const struct cldc_call_opts *copts)
{
	struct cldc_msg *msg;

	if (!sess->confirmed)
		return -EINVAL;

	/* create END-SESS message */
	msg = cldc_new_msg(sess, copts, cmo_end_sess,
			   sizeof(struct cld_msg_hdr));
	if (!msg)
		return -ENOMEM;

	msg->cb = end_sess_cb;

	return sess_send(sess, msg);
}

static ssize_t new_sess_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, bool ok)
{
	struct cldc_session *sess = msg->sess;
	const struct cld_msg_resp *resp = resp_p;
	enum cle_err_codes resp_rc = CLE_OK;

	if (!ok)
		resp_rc = CLE_TIMEOUT;
	else
		resp_rc = GUINT32_FROM_LE(resp->code);

	if (resp_rc == CLE_OK)
		sess->confirmed = true;

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

int cldc_new_sess(const struct cldc_ops *ops,
		  const struct cldc_call_opts *copts,
		  const void *addr, size_t addr_len,
		  const char *user, const char *secret_key,
		  void *private,
		  struct cldc_session **sess_out)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct timeval tv;

	if (addr_len > sizeof(sess->addr))
		return -EINVAL;

	if (!user || !*user || !secret_key || !*secret_key)
		return -EINVAL;
	if (strlen(user) >= sizeof(sess->user))
		return -EINVAL;
	if (strlen(secret_key) >= sizeof(sess->secret_key))
		return -EINVAL;

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return -ENOMEM;

#if 0
	sess->verbose = true;
#endif

	sess->private = private;
	sess->ops = ops;
	sess->act_log = ops->printf ? ops->printf : cldc_log;
	sess->fh = g_array_sized_new(FALSE, TRUE, sizeof(struct cldc_fh), 16);
	strcpy(sess->user, user);
	strcpy(sess->secret_key, secret_key);

	/* create random SID, next_seqid_out */
	__cld_rand64(sess->sid);
	__cld_rand64(&sess->next_seqid_out);

	/* init other session vars */
	memcpy(sess->addr, addr, addr_len);
	sess->addr_len = addr_len;

	/* create NEW-SESS message */
	msg = cldc_new_msg(sess, copts, cmo_new_sess,
			   sizeof(struct cld_msg_hdr));
	if (!msg) {
		sess_free(sess);
		return -ENOMEM;
	}

	msg->cb = new_sess_cb;

	/* save session */
	*sess_out = sess;

	gettimeofday(&tv, NULL);
	sess->expire_time = tv.tv_sec + CLDC_SESS_EXPIRE;

	sess->ops->timer_ctl(sess->private, true, sess_timer, sess,
			     CLDC_MSG_RETRY);

	return sess_send(sess, msg);
}

/*
 * Force-clean the slate in the library. This may leave the server confused.
 */
void cldc_kill_sess(struct cldc_session *sess)
{
	sess->ops->timer_ctl(sess->private, false, NULL, NULL, 0);
	sess_free(sess);
}

static ssize_t generic_end_cb(struct cldc_msg *msg, const void *resp_p,
			      size_t resp_len, bool ok)
{
	const struct cld_msg_resp *resp = resp_p;
	enum cle_err_codes resp_rc = CLE_OK;

	if (!ok)
		resp_rc = CLE_TIMEOUT;
	else
		resp_rc = GUINT32_FROM_LE(resp->code);

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

int cldc_nop(struct cldc_session *sess, const struct cldc_call_opts *copts)
{
	struct cldc_msg *msg;

	if (!sess->confirmed)
		return -EINVAL;

	/* create NOP message */
	msg = cldc_new_msg(sess, copts, cmo_nop, sizeof(struct cld_msg_hdr));
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	return sess_send(sess, msg);
}

int cldc_del(struct cldc_session *sess, const struct cldc_call_opts *copts,
	     const char *pathname)
{
	struct cldc_msg *msg;
	struct cld_msg_del *del;
	void *p;
	size_t plen;

	if (!sess->confirmed)
		return -EINVAL;

	/* first char must be slash */
	if (*pathname != '/')
		return -EINVAL;

	plen = strlen(pathname);
	if (plen > 65530)
		return -EINVAL;

	/* create DEL message */
	msg = cldc_new_msg(sess, copts, cmo_del,
			   sizeof(struct cld_msg_del) + strlen(pathname));
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	/* fill in DEL-specific name_len, name info */
	del = (struct cld_msg_del *) msg->data;
	del->name_len = GUINT16_TO_LE(plen);
	p = del;
	p += sizeof(struct cld_msg_del);
	memcpy(p, pathname, plen);

	return sess_send(sess, msg);
}

static ssize_t open_end_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, bool ok)
{
	const struct cld_msg_open_resp *resp = resp_p;
	struct cldc_fh *fh = msg->cb_private;
	enum cle_err_codes resp_rc = CLE_OK;

	if (resp_len < sizeof(*resp))
		return -EINVAL;

	if (!ok)
		resp_rc = CLE_TIMEOUT;
	else
		resp_rc = GUINT32_FROM_LE(resp->resp.code);

	if (resp_rc == CLE_OK) {
		fh->fh_le = resp->fh;
		fh->valid = true;
	}

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

int cldc_open(struct cldc_session *sess,
	      const struct cldc_call_opts *copts,
	      const char *pathname, uint32_t open_mode,
	      uint32_t events, struct cldc_fh **fh_out)
{
	struct cldc_msg *msg;
	struct cld_msg_open *open;
	struct cldc_fh fh, *fhtmp;
	void *p;
	size_t plen;
	int fh_idx;

	*fh_out = NULL;

	if (!sess->confirmed)
		return -EINVAL;

	/* first char must be slash */
	if (*pathname != '/')
		return -EINVAL;

	plen = strlen(pathname);
	if (plen > 65530)
		return -EINVAL;

	/* create OPEN message */
	msg = cldc_new_msg(sess, copts, cmo_open,
			   sizeof(struct cld_msg_open) + strlen(pathname));
	if (!msg)
		return -ENOMEM;

	/* add fh to fh table; get pointer to new fh */
	memset(&fh, 0, sizeof(fh));
	fh.sess = sess;
	fh_idx = sess->fh->len;
	g_array_append_val(sess->fh, fh);

	fhtmp = &g_array_index(sess->fh, struct cldc_fh, fh_idx);

	msg->cb = open_end_cb;
	msg->cb_private = fhtmp;

	/* fill in OPEN-specific info */
	open = (struct cld_msg_open *) msg->data;
	open->mode = GUINT32_TO_LE(open_mode);
	open->events = GUINT32_TO_LE(events);
	open->name_len = GUINT16_TO_LE(plen);
	p = open;
	p += sizeof(struct cld_msg_open);
	memcpy(p, pathname, plen);

	*fh_out = fhtmp;

	return sess_send(sess, msg);
}

int cldc_close(struct cldc_fh *fh, const struct cldc_call_opts *copts)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_close *close;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create CLOSE message */
	msg = cldc_new_msg(sess, copts, cmo_close,
			   sizeof(struct cld_msg_close));
	if (!msg)
		return -ENOMEM;

	/* mark FH as invalid from this point forward */
	fh->valid = false;

	msg->cb = generic_end_cb;

	/* fill in CLOSE-specific fh info */
	close = (struct cld_msg_close *) msg->data;
	close->fh = fh->fh_le;

	return sess_send(sess, msg);
}

int cldc_lock(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	      uint32_t lock_flags, bool wait_for_lock)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_lock *lock;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create LOCK message */
	msg = cldc_new_msg(sess, copts,
			   wait_for_lock ? cmo_lock : cmo_trylock,
			   sizeof(struct cld_msg_lock));
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	/* fill in LOCK-specific info */
	lock = (struct cld_msg_lock *) msg->data;
	lock->fh = fh->fh_le;
	lock->flags = GUINT32_TO_LE(lock_flags);

	return sess_send(sess, msg);
}

int cldc_unlock(struct cldc_fh *fh, const struct cldc_call_opts *copts)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_unlock *unlock;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create UNLOCK message */
	msg = cldc_new_msg(sess, copts, cmo_unlock,
			   sizeof(struct cld_msg_unlock));
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	/* fill in UNLOCK-specific info */
	unlock = (struct cld_msg_unlock *) msg->data;
	unlock->fh = fh->fh_le;

	return sess_send(sess, msg);
}

int cldc_put(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	     const void *data, size_t data_len)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_put *put;
	struct cldc_msg *datamsg[CLDC_MAX_DATA_PKTS];
	int n_pkts, i, copy_len;
	const void *p;
	size_t data_len_left = data_len;

	if (!data || !data_len || data_len > CLDC_MAX_DATA_SZ)
		return -EINVAL;

	n_pkts = (data_len / CLDC_MAX_DATA_PKT_SZ);
	if (data_len % CLDC_MAX_DATA_PKT_SZ)
		n_pkts++;
	n_pkts++;			/* add one for terminator segment */

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create PUT message */
	msg = cldc_new_msg(sess, copts, cmo_put, sizeof(struct cld_msg_put));
	if (!msg)
		return -ENOMEM;

	put = (struct cld_msg_put *) msg->data;
	put->fh = fh->fh_le;
	__cld_rand64(&put->strid);
	put->data_size = GUINT32_TO_LE(data_len);

	memset(datamsg, 0, sizeof(datamsg));

	p = data;
	for (i = 0; i < n_pkts; i++) {
		struct cld_msg_data *dm;
		void *q;

		/* create DATA message for this segment */
		datamsg[i] = cldc_new_msg(sess, copts, cmo_data_s,
					  CLDC_MAX_DATA_PKT_SZ);
		if (!datamsg[i])
			goto err_out;

		if (i == (n_pkts - 1))
			datamsg[i]->cb = generic_end_cb;

		dm = (struct cld_msg_data *) datamsg[i]->data;
		q = dm;
		q += sizeof(struct cld_msg_data);

		copy_len = MIN(CLDC_MAX_DATA_PKT_SZ, data_len_left);
		memcpy(q, p, copy_len);

		p += copy_len;
		data_len_left -= copy_len;

		dm->strid = put->strid;
		dm->seg = GUINT32_TO_LE(i);
		dm->seg_len = GUINT32_TO_LE(copy_len);
	}

	sess_send(sess, msg);
	for (i = 0; i < n_pkts; i++)
		sess_send(sess, datamsg[i]);

	return 0;

err_out:
	for (i = 0; i < n_pkts; i++)
		if (datamsg[i])
			free(datamsg[i]);
	free(msg);
	return -ENOMEM;
}

#undef XC32
#undef XC64
#define XC32(name) \
	o->name = GUINT32_FROM_LE(resp->name)
#define XC64(name) \
	o->name = GUINT64_FROM_LE(resp->name)

static ssize_t get_end_cb(struct cldc_msg *msg, const void *resp_p,
			  size_t resp_len, bool ok)
{
	const struct cld_msg_get_resp *resp = resp_p;
	enum cle_err_codes resp_rc = CLE_OK;
	struct cld_msg_get_resp *o = NULL;

	if (!ok)
		resp_rc = CLE_TIMEOUT;
	else
		resp_rc = GUINT32_FROM_LE(resp->resp.code);

	if (resp_rc == CLE_OK) {
		o = &msg->copts.u.get.resp;

		msg->copts.op = cmo_get;

		/* copy-and-swap */
		XC64(inum);
		XC32(ino_len);
		XC32(size);
		XC64(version);
		XC64(time_create);
		XC64(time_modify);
		XC32(flags);

		/* copy inode name */
		if (o->ino_len <= sizeof(CLD_INODE_NAME_MAX)) {
			const void *p;
			p = resp;
			p += sizeof(struct cld_msg_get_resp);
			memcpy(&msg->copts.u.get.inode_name, p, o->ino_len);
		} else {
			o->ino_len = 0;		/* Probably full of garbage */
		}
	}

	/* if error or get-meta, return immediately with response */
	if ((resp_rc != CLE_OK) || (resp->resp.hdr.op == cmo_get_meta)) {
		if (msg->copts.cb)
			return msg->copts.cb(&msg->copts, resp_rc);
		return 0;
	}

	sess_stream_open(msg->sess, resp->strid, o->size,
			 &msg->copts);

	return 0;
}
#undef XC

int cldc_get(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	     bool metadata_only)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_get *get;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create GET message */
	msg = cldc_new_msg(sess, copts, cmo_get,
			   sizeof(struct cld_msg_get));
	if (!msg)
		return -ENOMEM;

	msg->cb = get_end_cb;

	/* fill in GET-specific info */
	get = (struct cld_msg_get *) msg->data;
	get->fh = fh->fh_le;

	return sess_send(sess, msg);
}

int cldc_dirent_count(const void *data, size_t data_len)
{
	const void *p = data;
	size_t tmp_len = data_len;
	size_t str_len, rec_len, pad, total_len;
	const uint16_t *tmp16;
	int count = 0;

	while (tmp_len > 0) {
		if (tmp_len < 2)
			return -2;

		tmp16		= p;
		str_len		= GUINT16_FROM_LE(*tmp16);
		rec_len		= str_len + 2;
		pad		= CLD_ALIGN8(rec_len);
		total_len	= rec_len + pad;

		if (total_len > tmp_len)
			return -2;

		count++;

		p += total_len;
		tmp_len -= total_len;
	}

	return count;
}

static int dirent_length(const void *buf, size_t buflen, size_t *str_len_out)
{
	size_t str_len, rec_len, pad, total_len;
	const uint16_t *tmp16;

	if (buflen < 2)
		return -1;

	tmp16		= buf;
	str_len		= GUINT16_FROM_LE(*tmp16);
	rec_len		= str_len + 2;
	pad		= CLD_ALIGN8(rec_len);
	total_len	= rec_len + pad;

	if (total_len > buflen)
		return -1;

	if (str_len_out)
		*str_len_out = str_len;

	return total_len;
}

int cldc_dirent_first(struct cld_dirent_cur *dc)
{
	int dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	return 0;
}

int cldc_dirent_next(struct cld_dirent_cur *dc)
{
	int dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	dc->p += dirent_len;
	dc->tmp_len -= dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	return 0;
}

void cldc_dirent_cur_init(struct cld_dirent_cur *dc, const void *buf, size_t buflen)
{
	memset(dc, 0, sizeof(*dc));
	dc->p = buf;
	dc->tmp_len = buflen;
}

void cldc_dirent_cur_fini(struct cld_dirent_cur *dc)
{
	/* do nothing */
}

char *cldc_dirent_name(struct cld_dirent_cur *dc)
{
	const uint16_t *tmp16 = dc->p;
	size_t str_len = GUINT16_FROM_LE(*tmp16);
	char *s;

	s = malloc(str_len + 1);
	if (!s)
		return NULL;
	
	memcpy(s, dc->p + 2, str_len);
	s[str_len] = 0;

	return s;
}

