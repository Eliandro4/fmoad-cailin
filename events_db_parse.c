/*
 * Embedded event->sample database reader.
 *
 * events_db_bin[] is generated from events_db.bin by `xxd -i` (see Makefile)
 * and contains the per-bank (event_path -> sample_filename) mapping derived
 * from the FMOD JSON manifests.  This lets the loader resolve events natively
 * without the manifest files present at runtime.
 *
 * See events_db.h for the on-disk layout.  The reader walks the blob and
 * returns pointers into it (the blob is const and lives for the program's
 * lifetime), so no allocation is performed.
 */
#include <stdlib.h>
#include <string.h>
#include "events_db.h"

extern unsigned char events_db_bin[];
extern unsigned int events_db_bin_len;

static uint32_t db_u32(const uint8_t **pp)
{
	uint32_t v;
	memcpy(&v, *pp, sizeof(v));
	*pp += sizeof(v);
	return v;
}

int events_db_find(const char *bankname, struct bank_events *out)
{
	const uint8_t *p = (const uint8_t *)events_db_bin;
	const uint8_t *end = p + events_db_bin_len;
	uint32_t nbanks = db_u32(&p);

	for (uint32_t b = 0; b < nbanks && p < end; b++) {
		uint32_t bnlen = db_u32(&p);
		const char *bn = (const char *)p;
		p += bnlen;
		uint32_t nev = db_u32(&p);

		if ((size_t)bnlen == strlen(bankname) &&
		    memcmp(bn, bankname, bnlen) == 0) {
			out->bankname = bn;
			out->count = nev;
			if (nev == 0) {
				out->entries = NULL;
				return 1;
			}
			/* Allocate the entry array once and point into the blob. */
			struct event_entry *arr =
				(struct event_entry *)malloc(nev * sizeof(*arr));
			if (!arr)
				return 0;
			for (uint32_t e = 0; e < nev && p < end; e++) {
				uint32_t plen = db_u32(&p);
				char *ps = malloc(plen + 1);
				memcpy(ps, p, plen);
				ps[plen] = '\0';
				arr[e].path = ps;
				p += plen;
				uint32_t flen = db_u32(&p);
				char *fs = malloc(flen + 1);
				memcpy(fs, p, flen);
				fs[flen] = '\0';
				arr[e].file = fs;
				p += flen;
			}
			out->entries = arr;
			return 1;
		}

		/* Skip this bank's events. */
		for (uint32_t e = 0; e < nev && p < end; e++) {
			uint32_t plen = db_u32(&p); p += plen;
			uint32_t flen = db_u32(&p); p += flen;
		}
	}
	return 0;
}

void events_db_free(struct bank_events *be)
{
	if (be && be->entries) {
		for (uint32_t i = 0; i < be->count; i++) {
			free((void *)be->entries[i].path);
			free((void *)be->entries[i].file);
		}
		free(be->entries);
		be->entries = NULL;
	}
}
