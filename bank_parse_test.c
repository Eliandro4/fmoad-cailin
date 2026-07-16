#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bank_parse.h"

/*
 * Standalone test for the native STDT (String Data Table) parser and the
 * global GUID<->path registry described in docs/bank_parsing.md.
 *
 * Usage:
 *   cc -o /tmp/bank_parse_test bank_parse_test.c bank_parse.c -o ...
 *   /tmp/bank_parse_test Master\ Bank.strings.bank
 *
 * The test parses the .strings.bank, prints every Event GUID -> path entry
 * it recovered from the packed radix trie, and then round-trips a few of
 * them through the global strmap (add -> lookup path -> lookup guid).
 */

static uint8_t *read_file(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) { perror("fopen"); return NULL; }
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)sz);
	if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		fclose(fp); free(buf); return NULL;
	}
	fclose(fp);
	*out_len = (size_t)sz;
	return buf;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <bank.strings.bank>\n", argv[0]);
		return 2;
	}

	size_t size;
	uint8_t *data = read_file(argv[1], &size);
	if (!data) return 1;

	struct fev_bank bank;
	memset(&bank, 0, sizeof(bank));

	if (fev_parse_strings(data, size, &bank) < 0) {
		fprintf(stderr, "fev_parse_strings failed\n");
		free(data);
		return 1;
	}

	printf("strings=%u\n", bank.n_strings);
	for (uint32_t i = 0; i < bank.n_strings; i++) {
		char g[37];
		fev_guid_str(&bank.strings[i].guid, g, sizeof(g));
		printf("%s -> %s\n", g,
		       bank.strings[i].path ? bank.strings[i].path : "(null)");
	}

	/* Exercise the global registry: merge and round-trip. */
	fev_strmap_add_bank(&bank);
	struct fev_strmap *sm = fev_strmap_get();
	printf("strmap_count=%u\n", sm->count);

	int failures = 0;
	for (uint32_t i = 0; i < bank.n_strings && i < 16; i++) {
		const char *path = fev_strmap_lookup_path(&bank.strings[i].guid);
		if (!path || (bank.strings[i].path && strcmp(path, bank.strings[i].path) != 0)) {
			fprintf(stderr, "path round-trip mismatch for entry %u\n", i);
			failures++;
			continue;
		}
		char gstr[37];
		fev_guid_str(&bank.strings[i].guid, gstr, sizeof(gstr));
		struct fev_guid back;
		if (!fev_strmap_lookup_guid(gstr, &back)) {
			fprintf(stderr, "guid lookup failed for %s\n", gstr);
			failures++;
			continue;
		}
		if (!fev_guid_eq(&back, &bank.strings[i].guid)) {
			fprintf(stderr, "guid round-trip mismatch for %s\n", gstr);
			failures++;
		}
	}

	fev_bank_free(&bank);
	free(data);

	if (failures) {
		fprintf(stderr, "%d round-trip failures\n", failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
