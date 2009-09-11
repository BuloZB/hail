
#include "chunkd-config.h"
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <locale.h>
#include <chunkc.h>
#include "test.h"

enum {
	N_TEST_OBJS		= 10000,
};

static void test(int n_objects, bool encrypt)
{
	struct st_keylist *klist;
	struct st_client *stc;
	int port;
	bool rcb;
	char val[] = "my first value";
	char key[64] = "";
	int i;
	GList *keys = NULL, *tmpl;
	char *k;
	struct timeval ta, tb;

	port = stc_readport(encrypt ? TEST_PORTFILE_SSL : TEST_PORTFILE);
	OK(port > 0);

	stc = stc_new(TEST_HOST, port, TEST_USER, TEST_USER_KEY, encrypt);
	OK(stc);

	fprintf(stderr, "      lotsa-objects syncing...\n");
	sync();

	gettimeofday(&ta, NULL);

	/* store object */
	for (i = 0; i < n_objects; i++) {
		sprintf(key, "%x000000", i);
		rcb = stc_put_inline(stc, key, val, strlen(val));
		OK(rcb);

		keys = g_list_prepend(keys, strdup(key));
	}

	gettimeofday(&tb, NULL);

	printdiff(&ta, &tb, n_objects,
		  encrypt ? "lotsa-objects SSL PUT": "lotsa-objects PUT", "ops");

	fprintf(stderr, "      lotsa-objects syncing...\n");
	sync();

	gettimeofday(&ta, NULL);

	/* verify keylist is received */
	klist = stc_keys(stc);
	OK(klist);
	OK(klist->contents);

	gettimeofday(&tb, NULL);

	printdiff(&ta, &tb, n_objects,
		  encrypt ? "lotsa-objects SSL Key list": "lotsa-objects Key list", "entries");

	i = 0;
	tmpl = klist->contents;
	while (tmpl) {
		i++;
		tmpl = tmpl->next;
	}

	OK(i == n_objects);

	stc_free_keylist(klist);

	gettimeofday(&ta, NULL);

	/* get objects */
	for (tmpl = keys; tmpl; tmpl = tmpl->next) {
		size_t len;
		void *mem;

		k = tmpl->data;
		len = 0;

		mem = stc_get_inline(stc, k, &len);
		OK(mem);
		OK(len == strlen(val));
		OK(!memcmp(val, mem, strlen(val)));

		free(mem);
	}

	gettimeofday(&tb, NULL);

	printdiff(&ta, &tb, n_objects,
		  encrypt ? "lotsa-objects SSL GET": "lotsa-objects GET", "ops");

	gettimeofday(&ta, NULL);

	/* delete object */
	for (tmpl = keys; tmpl; tmpl = tmpl->next) {
		k = tmpl->data;
		rcb = stc_del(stc, k);
		OK(rcb);

		free(k);
	}

	gettimeofday(&tb, NULL);

	printdiff(&ta, &tb, n_objects,
		  encrypt ? "lotsa-objects SSL DELETE": "lotsa-objects DELETE", "ops");

	fprintf(stderr, "      lotsa-objects syncing...\n");
	sync();

	g_list_free(keys);

	stc_free(stc);
}

int main(int argc, char *argv[])
{
	int n_objects = N_TEST_OBJS;

	setlocale(LC_ALL, "C");

	stc_init();
	SSL_library_init();
	SSL_load_error_strings();

	if (argc == 2 && (atoi(argv[1]) > 0)) {
		n_objects = atoi(argv[1]);
		fprintf(stderr, "testing %d objects...\n", n_objects);
	}

	test(n_objects, false);
	test(n_objects, true);

	return 0;
}

