#ifndef __CLDC_H__
#define __CLDC_H__

#include <sys/types.h>
#include <cld_msg.h>

struct cldc_msg;
struct cldc_session;

struct cldc_msg {
	uint8_t		msgid[CLD_MSGID_SZ];	/* message id */

	struct cldc_session *sess;

	ssize_t		(*cb)(struct cldc_msg *);
	void		*cb_private;

	bool		done;

	time_t		expire_time;

	int		data_len;
	uint8_t		data[0];

	int		retries;
};

struct cldc_session {
	uint8_t		sid[CLD_SID_SZ];	/* client id */

	uint8_t		addr[64];		/* server address */
	size_t		addr_len;

	GHashTable	*out_msg;
	time_t		msg_scan_time;

	uint64_t	next_msgid;

	bool		confirmed;
};

struct cldc {
	/* public: set by app */
	void		*private;
	ssize_t		(*pkt_send)(void *private,
				const void *addr, size_t addrlen,
				const void *buf, size_t buflen);

	/* private: managed by lib */
	GHashTable	*sessions;
};

struct cld_client {
	int			fd;		/* UDP socket */

	char			host[256];
	struct sockaddr_in6	addr;
	socklen_t		addrlen;

	struct sockaddr_in6	local_addr;
	socklen_t		local_addrlen;
	int			local_port;
};

extern int cldcli_init(void);
extern void cldcli_free(struct cld_client *);
extern struct cld_client *cldcli_new(const char *remote_host, int remote_port,
				   int local_port);

extern int cldc_new_sess(struct cldc *cldc, const void *addr, size_t addr_len,
		  struct cldc_session **sess_out);

#endif /* __CLDC_H__ */
