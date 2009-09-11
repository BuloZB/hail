#ifndef __STC_H__
#define __STC_H__

#include <sys/types.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

struct st_object {
	char		*name;
	char		*time_mod;
	char		*etag;
	uint64_t	size;
	char		*owner;
};

struct st_keylist {
	char		*name;
	GList		*contents;
};

struct st_client {
	char		*host;
	char		*user;
	char		*key;
	bool		verbose;

	int		fd;

	SSL_CTX		*ssl_ctx;
	SSL		*ssl;
};

extern void stc_free(struct st_client *stc);
extern void stc_free_keylist(struct st_keylist *keylist);
extern void stc_free_object(struct st_object *obj);
extern void stc_init(void);

extern struct st_client *stc_new(const char *service_host, int port,
				 const char *user, const char *secret_key,
				 bool encrypt);

extern bool stc_get(struct st_client *stc, const char *key,
	     size_t (*write_cb)(void *, size_t, size_t, void *),
	     void *user_data);
extern void *stc_get_inline(struct st_client *stc,
			    const char *key, size_t *len);
extern bool stc_get_start(struct st_client *stc, const char *key, int *pfd,
			  uint64_t *len);
extern size_t stc_get_recv(struct st_client *stc, void *data, size_t len);
extern bool stc_put(struct st_client *stc, const char *key,
	     size_t (*read_cb)(void *, size_t, size_t, void *),
	     uint64_t len, void *user_data);
extern bool stc_put_start(struct st_client *stc, const char *key,
	     uint64_t cont_len, int *pfd);
extern size_t stc_put_send(struct st_client *stc, void *data, size_t len);
extern bool stc_put_sync(struct st_client *stc);
extern bool stc_put_inline(struct st_client *stc, const char *key,
			   void *data, uint64_t len);
extern bool stc_del(struct st_client *stc, const char *key);
extern bool stc_ping(struct st_client *stc);

extern struct st_keylist *stc_keys(struct st_client *stc);

extern int stc_readport(const char *fname);

#endif /* __STC_H__ */
