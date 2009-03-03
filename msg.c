
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

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "cld.h"

enum {
	CLD_MAX_UDP_SEG		= 1024,
};

struct pathname_info {
	char		*dir;
	size_t		dir_len;
	char		*base;
	size_t		base_len;
};

static void session_timeout(int fd, short events, void *userdata);

static bool valid_inode_name(const char *name, size_t name_len)
{
	if (!name || !*name || !name_len)
		return false;
	if (strnlen(name, name_len) != name_len)
		return false;
	if (name[0] != '/')
		return false;
	if (name_len > 1 && name[name_len - 1] == '/')
		return false;
	if (memmem(name, name_len, "//", 2))
		return false;
	return true;
}

static void pathname_parse(char *path, size_t path_len,
			   struct pathname_info *pinfo)
{
	char *lsl;
	int ofs;

	lsl = memrchr(path, path_len, '/');
	ofs = lsl - path + 1;

	pinfo->dir = path;
	pinfo->dir_len = ofs - 1;
	pinfo->base = path + ofs;
	pinfo->base_len = path_len - ofs;
}

static int dirent_find(const void *data, size_t data_len,
		       const char *name, size_t name_len,
		       int *ofs_out, size_t *ent_len_out)
{
	const void *p = data;
	size_t tmp_len = data_len;
	size_t str_len, rec_len, pad, total_len;
	const uint16_t *tmp16;
	long ofs;

	while (tmp_len > 0) {
		if (tmp_len < 2)
			return -2;

		tmp16		= p;
		str_len		= GUINT16_FROM_LE(*tmp16);
		rec_len		= str_len + 2;
		pad		= ALIGN8(rec_len);
		total_len	= rec_len + pad;

		p += 2;
		tmp_len -= 2;

		if (total_len > tmp_len)
			return -2;

		if ((name_len == str_len) &&
		    !memcmp(p, name, name_len))
			break;

		p += total_len;
		tmp_len -= total_len;
	}

	if (!tmp_len)
		return -1;

	ofs = (p - data) - 2;

	if (ofs_out)
		*ofs_out = (int) ofs;
	if (ent_len_out)
		*ent_len_out = total_len;

	return 0;
}

static bool dirdata_delete(void *data, size_t *data_len_io,
			   const char *name, size_t name_len)
{
	int rc, ofs = -1;
	size_t ent_len = 0, new_len;
	size_t data_len = *data_len_io;

	rc = dirent_find(data, data_len, name, name_len, &ofs, &ent_len);
	if (rc)
		return false;

	new_len = data_len - ent_len;

	if ((ofs + ent_len) < data_len)
		memmove(data + ofs,
			data + ofs + ent_len,
			data_len - (ofs + ent_len));

	*data_len_io = new_len;
	return true;
}

static bool dirdata_append(void **data, size_t *data_len,
			   const char *name, size_t name_len)
{
	size_t rec_alloc, new_len, pad, orig_len, rec_len;
	void *mem, *p;
	uint16_t *raw_len;

	rec_len		= name_len + 2;
	pad		= ALIGN8(rec_len);
	rec_alloc	= rec_len + pad;
	orig_len	= *data_len;
	new_len		= orig_len + rec_alloc;

	mem = realloc(*data, new_len);
	if (!mem)
		return false;

	/* store 16-bit string length, little endian */
	p = mem + orig_len;
	raw_len = p;
	*raw_len = GUINT16_TO_LE(name_len);
	p += sizeof(uint16_t);

	/* store name, zero pad area (if any) */
	memcpy(p, name, name_len);
	if (pad)
		memset(p + name_len, 0, pad);

	*data = mem;
	*data_len = new_len;
	return true;
}

guint sess_hash(gconstpointer v)
{
	const struct session *sess = v;
	const uint64_t *tmp = (const uint64_t *) sess->clid;

	return (guint) *tmp;
}

gboolean sess_equal(gconstpointer _a, gconstpointer _b)
{
	const struct session *a = _a;
	const struct session *b = _b;

	return (memcmp(a->clid, b->clid, CLD_CLID_SZ) == 0);
}

static struct session *session_new(void)
{
	struct session *sess;

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return NULL;

	sess->handles = g_array_new(FALSE, FALSE, sizeof(uint64_t));

	evtimer_set(&sess->timer, session_timeout, sess);

	return sess;
}

static void session_encode(struct raw_session *raw, const struct session *sess)
{
	uint64_t *hsrc, *hdest;
	int i;
	void *p;

	memcpy(raw, sess, CLD_CLID_SZ + CLD_IPADDR_SZ);
	raw->last_contact = GUINT64_TO_LE(sess->last_contact);
	raw->next_fh = GUINT64_TO_LE(sess->next_fh);
	raw->n_handles = GUINT32_TO_LE(sess->handles->len);

	hsrc = (uint64_t *) sess->handles->data;

	p = raw;
	p += sizeof(*raw);
	hdest = p;

	for (i = 0; i < sess->handles->len; i++)
		hdest[i] = GUINT64_TO_LE(hsrc[i]);
}

static struct raw_session *session_new_raw(const struct session *sess)
{
	struct raw_session *raw_sess;
	size_t alloc_len;

	alloc_len = sizeof(*raw_sess) + (sizeof(uint64_t) * sess->handles->len);
	raw_sess = malloc(alloc_len);
	if (!raw_sess)
		return NULL;

	session_encode(raw_sess, sess);

	return raw_sess;
}

static bool inode_append(struct raw_inode **ino, struct raw_handle *h)
{
	size_t new_len, orig_len;
	void *mem, *p;
#if 0
	uint32_t n_handles;
#endif

	orig_len	= raw_ino_size(*ino);
	new_len		= orig_len + sizeof(struct raw_handle_key);

	mem = realloc(*ino, new_len);
	if (!mem)
		return false;

	p = mem + orig_len;
	memcpy(p, h, sizeof(struct raw_handle_key));

#if 0
	n_handles = GUINT32_FROM_LE((*ino)->n_handles);
	(*ino)->n_handles = GUINT32_TO_LE(n_handles + 1);
#endif

	return true;
}

bool msg_get(struct server_socket *sock, DB_TXN *txn,
	     const struct client *cli, struct session *sess,
	     uint8_t *raw_msg, size_t msg_len, bool metadata_only)
{
	struct cld_msg_get *msg = (struct cld_msg_get *) raw_msg;
	struct cld_msg_get_resp *resp;
	size_t resp_len;
	uint64_t fh;
	struct raw_handle *h = NULL;
	struct raw_inode *inode = NULL;
	enum cle_err_codes resp_rc = CLE_OK;
	cldino_t inum;
	uint32_t name_len;
	uint32_t data_size;
	void *data_mem = NULL;
	size_t data_mem_len = 0;
	int rc;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	fh = GUINT64_FROM_LE(msg->fh);

	rc = cldb_handle_get(txn, sess->clid, fh, &h, 0);
	if (rc) {
		resp_rc = CLE_FH_INVAL;
		goto err_out;
	}

	inum = cldino_from_le(h->inum);

	rc = cldb_inode_get(txn, inum, &inode, false, 0);
	if (rc) {
		resp_rc = CLE_INODE_INVAL;
		goto err_out;
	}

	name_len = GUINT32_FROM_LE(inode->ino_len);
	data_size = GUINT32_FROM_LE(inode->size);

	resp_len = sizeof(*resp) + name_len;
	resp = alloca(resp_len);

	resp_copy(&resp->hdr, &msg->hdr);
	resp->inum = GUINT64_TO_LE(inum);
	memcpy(&resp->ino_len, &inode->ino_len,
	       (sizeof(struct raw_inode) - sizeof(inode->inum)) + name_len);

	udp_tx(sock, cli, &resp, sizeof(resp));

	if (!metadata_only) {
		int i, seg_len;
		void *p;
		char dbuf[CLD_MAX_UDP_SEG];
		struct cld_msg_data_resp *dr =
			(struct cld_msg_data_resp *) &dbuf;

		rc = cldb_data_get(txn, inum, &data_mem, &data_mem_len,
				   true, false);
		if (rc) {
			resp_rc = CLE_DB_ERR;
			goto err_out;
		}

		resp_copy(&dr->hdr, &msg->hdr);
		dr->hdr.op = cmo_data;
		i = 0;
		p = data_mem;

		while (data_mem_len > 0) {
			seg_len = MIN(CLD_MAX_UDP_SEG, data_mem_len);

			seg_len -= sizeof(*dr);

			dr->seg = GUINT32_TO_LE(i);
			dr->seg_len = GUINT32_TO_LE(seg_len);
			memcpy(dbuf + sizeof(*dr), p, seg_len);

			i++;
			p += seg_len;
			data_mem_len -= seg_len;

			udp_tx(sock, cli, dr, seg_len + sizeof(*dr));
		}

		/* send terminating packet */
		dr->seg = GUINT32_TO_LE(i);
		dr->seg_len = 0;
		udp_tx(sock, cli, dr, sizeof(*dr));
	}

	free(h);
	free(inode);
	free(data_mem);
	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	free(h);
	free(inode);
	free(data_mem);
	return false;
}

bool msg_open(struct server_socket *sock, DB_TXN *txn, const struct client *cli,
	      struct session *sess, uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_open *msg = (struct cld_msg_open *) raw_msg;
	struct cld_msg_resp_open resp;
	char *name;
	struct raw_session *raw_sess = NULL;
	struct raw_inode *inode = NULL, *parent = NULL;
	struct raw_handle *h;
	int rc, name_len;
	bool create;
	struct pathname_info pinfo;
	void *parent_data = NULL;
	size_t parent_len;
	uint32_t msg_mode, msg_events;
	uint64_t fh;
	cldino_t inum;
	enum cle_err_codes resp_rc = CLE_OK;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	msg_mode = GUINT32_FROM_LE(msg->mode);
	msg_events = GUINT32_FROM_LE(msg->events);
	name_len = GUINT16_FROM_LE(msg->name_len);

	if (msg_len < (sizeof(*msg) + name_len))
		return false;

	name = (char *) raw_msg + sizeof(*msg);

	create = msg_mode & COM_CREATE;

	if (!valid_inode_name(name, name_len) || (create && name_len < 2)) {
		resp_rc = CLE_NAME_INVAL;
		goto err_out;
	}

	pathname_parse(name, name_len, &pinfo);

	/* read inode from db, if it exists */
	rc = cldb_inode_get_byname(txn, name, name_len, &inode, true, true);
	if (rc && ((rc != DB_NOTFOUND) || (!create))) {
		resp_rc = (rc == DB_NOTFOUND) ? CLE_INODE_INVAL : CLE_DB_ERR;
		goto err_out;
	}

	if (create) {
		/* create new in-memory inode */
		inode = cldb_inode_new(txn, pinfo.base, pinfo.base_len, 0);
		if (!inode) {
			syslog(LOG_CRIT, "out of memory");
			resp_rc = CLE_OOM;
			goto err_out;
		}

		/* read parent, to which we will add new child inode */
		rc = cldb_inode_get_byname(txn, pinfo.dir, pinfo.dir_len,
				    &parent, true, true);
		if (rc) {
			resp_rc = CLE_DB_ERR;
			goto err_out;
		}

		/* read parent inode data, if any */
		rc = cldb_data_get(txn, cldino_from_le(parent->inum),
				   &parent_data, &parent_len, true, true);
		if (rc) {
			resp_rc = CLE_DB_ERR;
			goto err_out;
		}

		/* append new record to inode's directory data */
		if (!dirdata_append(&parent_data, &parent_len,
				    pinfo.base, pinfo.base_len)) {
			syslog(LOG_CRIT, "out of memory");
			resp_rc = CLE_OOM;
			goto err_out;
		}

		/* write parent inode's updated directory data */
		rc = cldb_data_put(txn, cldino_from_le(parent->inum),
				   parent_data, parent_len, 0);
		if (rc) {
			resp_rc = CLE_DB_ERR;
			goto err_out;
		}

		parent->time_modify = GUINT64_TO_LE(current_time);
		parent->size = GUINT32_TO_LE(parent_len);
		parent->version = GUINT32_TO_LE(
			GUINT32_FROM_LE(parent->version) + 1);

		/* write parent inode */
		rc = cldb_inode_put(txn, parent, 0);
		if (rc) {
			resp_rc = CLE_DB_ERR;
			goto err_out;
		}
	}

	inum = cldino_from_le(inode->inum);

	/* alloc & init new handle; updates session's next_fh */
	h = cldb_handle_new(sess, inum, msg_mode, msg_events);
	if (!h) {
		syslog(LOG_CRIT, "out of memory");
		resp_rc = CLE_OOM;
		goto err_out;
	}

	fh = GUINT64_FROM_LE(h->fh);

	/* write newly created file handle */
	rc = cldb_handle_put(txn, h, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	g_array_append_val(sess->handles, fh);

	raw_sess = session_new_raw(sess);

	/* add handle to session, and to inode */
	if (!raw_sess || !inode_append(&inode, h)) {
		syslog(LOG_CRIT, "out of memory");
		resp_rc = CLE_OOM;
		goto err_out;
	}

	inode->time_modify = GUINT64_TO_LE(current_time);

	/* write inode */
	rc = cldb_inode_put(txn, inode, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	/* write session */
	rc = cldb_session_put(txn, raw_sess, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	free(parent_data);
	free(parent);
	free(inode);
	free(raw_sess);

	resp_copy(&resp.hdr, &msg->hdr);
	resp.code = GUINT32_TO_LE(CLE_OK);
	resp.fh = GUINT64_TO_LE(fh);
	udp_tx(sock, cli, &resp, sizeof(resp));

	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	free(parent_data);
	free(parent);
	free(inode);
	free(raw_sess);
	return false;
}

static void session_timeout(int fd, short events, void *userdata)
{
	struct session *sess = userdata;

	/* FIXME */
	(void) sess;
}

bool msg_new_cli(struct server_socket *sock, DB_TXN *txn,
		 const struct client *cli, uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_hdr *msg = (struct cld_msg_hdr *) raw_msg;
	DB *db = cld_srv.cldb.sessions;
	struct raw_session raw_sess;
	struct session *sess;
	DBT key, val;
	int rc;
	struct timeval tv;

	sess = session_new();
	if (!sess) {
		resp_err(sock, cli, msg, CLE_OOM);
		return false;
	}

	memcpy(&sess->clid, &msg->clid, sizeof(sess->clid));
	memcpy(&sess->addr, &cli->addr, sizeof(sess->addr));
	sess->addr_len = cli->addr_len;
	strncpy(sess->ipaddr, cli->addr_host, sizeof(sess->ipaddr));
	sess->last_contact = current_time;

	session_encode(&raw_sess, sess);

	memset(&key, 0, sizeof(key));
	memset(&val, 0, sizeof(val));

	key.data = &raw_sess.clid;
	key.size = sizeof(raw_sess.clid);

	val.data = &raw_sess;
	val.size = sizeof(raw_sess);

	rc = db->put(db, txn, &key, &val, DB_NOOVERWRITE);
	if (rc)
		goto err_out;

	g_hash_table_insert(cld_srv.sessions, sess->clid, sess);

	tv.tv_sec = CLD_CLI_TIMEOUT;
	tv.tv_usec = 0;
	if (evtimer_add(&sess->timer, &tv) < 0) {
		syslog(LOG_WARNING, "evtimer_add session_new failed");
		goto err_out;
	}

	resp_ok(sock, cli, msg);
	return true;

err_out:
	resp_err(sock, cli, msg, CLE_DB_ERR);
	free(sess);
	return false;
}

bool msg_put(struct server_socket *sock, DB_TXN *txn, const struct client *cli,
	     struct session *sess, uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_put *msg = (struct cld_msg_put *) raw_msg;
	uint64_t fh;
	struct raw_handle *h = NULL;
	struct raw_inode *inode = NULL;
	enum cle_err_codes resp_rc = CLE_OK;
	void *mem;
	int rc;
	cldino_t inum;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	fh = GUINT64_FROM_LE(msg->fh);

	rc = cldb_handle_get(txn, sess->clid, fh, &h, 0);
	if (rc) {
		resp_rc = CLE_FH_INVAL;
		goto err_out;
	}

	inum = cldino_from_le(h->inum);

	rc = cldb_inode_get(txn, inum, &inode, false, 0);
	if (rc) {
		resp_rc = CLE_INODE_INVAL;
		goto err_out;
	}

	mem = malloc(msg_len);
	if (!mem) {
		resp_rc = CLE_OOM;
		goto err_out;
	}

	memcpy(mem, raw_msg, msg_len);

	sess->put_q = g_list_append(sess->put_q, mem);

	free(h);
	free(inode);
	resp_ok(sock, cli, &msg->hdr);
	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	free(h);
	free(inode);
	return false;
}

static bool try_commit_data(struct server_socket *sock, DB_TXN *txn,
			struct session *sess, const struct client *cli,
			uint8_t *msgid, GList *pmsg_ent)
{
	struct cld_msg_put *pmsg = pmsg_ent->data;
	struct cld_msg_data *dmsg;
	GList *tmp, *tmp1;
	uint32_t data_size, tmp_size, tmp_seg;
	int last_seg, nseg, rc, i;
	struct raw_handle *h = NULL;
	struct raw_inode *inode = NULL;
	cldino_t inum;
	uint64_t fh;
	enum cle_err_codes resp_rc = CLE_OK;
	void *mem, *p, *q;
	struct cld_msg_data **darr;

	data_size = GUINT32_FROM_LE(pmsg->data_size);
	tmp_size = 0;
	last_seg = 0;
	nseg = 0;

	tmp = sess->data_q;
	while (tmp) {
		uint32_t tmp_seg;

		dmsg = tmp->data;
		tmp = tmp->next;

		if (memcmp(dmsg->hdr.msgid, msgid, 8))
			continue;

		tmp_seg = GUINT32_FROM_LE(dmsg->seg);
		if (tmp_seg > last_seg)
			last_seg = tmp_seg;

		tmp_size += GUINT32_FROM_LE(dmsg->seg_len);
		nseg++;
	}

	if (tmp_size < data_size)
		return true;		/* nothing to do */

	darr = alloca(nseg * sizeof(struct cld_msg_data *));
	memset(darr, 0, nseg * sizeof(struct cld_msg_data *));

	sess->put_q = g_list_delete_link(sess->put_q, pmsg_ent);

	tmp = sess->data_q;
	while (tmp) {
		dmsg = tmp->data;
		tmp1 = tmp;
		tmp = tmp->next;

		if (memcmp(dmsg->hdr.msgid, msgid, 8))
			continue;
		
		sess->data_q = g_list_delete_link(sess->data_q, tmp1);

		tmp_seg = GUINT32_FROM_LE(dmsg->seg);
		darr[tmp_seg] = dmsg;
	}
	
	fh = GUINT64_FROM_LE(pmsg->fh);

	rc = cldb_handle_get(txn, sess->clid, fh, &h, 0);
	if (rc) {
		resp_rc = CLE_FH_INVAL;
		goto err_out;
	}

	inum = cldino_from_le(h->inum);

	rc = cldb_inode_get(txn, inum, &inode, false, DB_RMW);
	if (rc) {
		resp_rc = CLE_INODE_INVAL;
		goto err_out;
	}

	p = mem = malloc(data_size);
	if (!mem) {
		resp_rc = CLE_OOM;
		goto err_out;
	}

	for (i = 0; i <= last_seg; i++) {
		dmsg = darr[i];
		q = dmsg;

		tmp_size = GUINT32_FROM_LE(dmsg->seg_len);
		memcpy(p, q + sizeof(*dmsg), tmp_size);
		p += tmp_size;

		free(dmsg);
	}

	rc = cldb_data_put(txn, inum, mem, data_size, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	inode->size = GUINT32_TO_LE(data_size);
	inode->version = GUINT32_TO_LE(
		GUINT32_FROM_LE(inode->version) + 1);
	inode->time_modify = GUINT64_TO_LE(current_time);

	rc = cldb_inode_put(txn, inode, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	resp_ok(sock, cli, (struct cld_msg_hdr *) pmsg);
	free(pmsg);
	free(h);
	free(inode);
	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) pmsg, resp_rc);
	free(pmsg);
	free(h);
	free(inode);
	return false;
}

bool msg_data(struct server_socket *sock, DB_TXN *txn, const struct client *cli,
	      struct session *sess, uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_data *msg = (struct cld_msg_data *) raw_msg;
	GList *tmp;
	void *mem = NULL;
	enum cle_err_codes resp_rc = CLE_OK;
	uint32_t seg_len;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	seg_len = GUINT32_FROM_LE(msg->seg_len);

	if (msg_len < (sizeof(*msg) + seg_len))
		return false;

	/* search for PUT message with same msgid; that is how we
	 * associate DATA messages with the initial PUT msg
	 */
	tmp = sess->put_q;
	while (tmp) {
		struct cld_msg_put *pmsg;

		pmsg = tmp->data;
		if (!memcmp(pmsg->hdr.msgid, msg->hdr.msgid,
			    sizeof(msg->hdr.msgid)))
			break;

		tmp = tmp->next;
	}

	if (!tmp) {
		resp_rc = CLE_DATA_INVAL;
		goto err_out;
	}

	mem = malloc(msg_len);
	if (!mem) {
		resp_rc = CLE_OOM;
		goto err_out;
	}

	memcpy(mem, raw_msg, msg_len);

	sess->data_q = g_list_append(sess->data_q, mem);

	udp_tx(sock, cli, msg, sizeof(*msg));

	return try_commit_data(sock, txn, sess, cli, msg->hdr.msgid, tmp);

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	return false;
}

bool msg_close(struct server_socket *sock, DB_TXN *txn,
	       const struct client *cli, struct session *sess,
	       uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_close *msg = (struct cld_msg_close *) raw_msg;
	uint64_t fh;
	int rc;
	enum cle_err_codes resp_rc = CLE_OK;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	fh = GUINT64_FROM_LE(msg->fh);

	rc = cldb_handle_del(txn, sess->clid, fh);
	if (rc) {
		if (rc == DB_NOTFOUND)
			resp_rc = CLE_FH_INVAL;
		else
			resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	resp_ok(sock, cli, (struct cld_msg_hdr *) msg);
	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	return false;
}

bool msg_del(struct server_socket *sock, DB_TXN *txn, const struct client *cli,
	     struct session *sess, uint8_t *raw_msg, size_t msg_len)
{
	struct cld_msg_del *msg = (struct cld_msg_del *) raw_msg;
	enum cle_err_codes resp_rc = CLE_OK;
	int rc, name_len;
	char *name;
	struct pathname_info pinfo;
	struct raw_inode *parent = NULL;
	void *parent_data = NULL;
	size_t parent_len;

	/* make sure input data as large as expected */
	if (msg_len < sizeof(*msg))
		return false;

	name_len = GUINT16_FROM_LE(msg->name_len);

	if (msg_len < (sizeof(*msg) + name_len))
		return false;

	name = (char *) raw_msg + sizeof(*msg);

	if (!valid_inode_name(name, name_len) || (name_len < 2)) {
		resp_rc = CLE_NAME_INVAL;
		goto err_out;
	}

	pathname_parse(name, name_len, &pinfo);

	/* read parent, to which we will add new child inode */
	rc = cldb_inode_get_byname(txn, pinfo.dir, pinfo.dir_len,
			    &parent, true, true);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	/* read parent inode data */
	rc = cldb_data_get(txn, cldino_from_le(parent->inum),
			   &parent_data, &parent_len, true, true);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	/* delete inode */
	rc = cldb_inode_del_byname(txn, name, name_len, true);
	if (rc) {
		if (rc == DB_NOTFOUND)
			resp_rc = CLE_INODE_INVAL;
		else
			resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	/* remove record from inode's directory data */
	if (!dirdata_delete(&parent_data, &parent_len,
			    pinfo.base, pinfo.base_len)) {
		syslog(LOG_WARNING, "dirent del failed");
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	/* write parent inode's updated directory data */
	rc = cldb_data_put(txn, cldino_from_le(parent->inum),
			   parent_data, parent_len, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	parent->time_modify = GUINT64_TO_LE(current_time);
	parent->size = GUINT32_TO_LE(parent_len);
	parent->version = GUINT32_TO_LE(
		GUINT32_FROM_LE(parent->version) + 1);

	/* write parent inode */
	rc = cldb_inode_put(txn, parent, 0);
	if (rc) {
		resp_rc = CLE_DB_ERR;
		goto err_out;
	}

	resp_ok(sock, cli, (struct cld_msg_hdr *) msg);
	free(parent);
	free(parent_data);
	return true;

err_out:
	resp_err(sock, cli, (struct cld_msg_hdr *) msg, resp_rc);
	free(parent);
	free(parent_data);
	return false;
}
