#ifndef BANK_PARSE_H
#define BANK_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Native FMOD Studio bank parser.
 *
 * Each .bank file (RIFF container) carries:
 *   - a BNKI chunk  : the bank GUID (and a 16-byte reserved block)
 *   - an EVTS list  : one EVNT entry per event, each with a 16-byte event
 *                     GUID and references to the waves it plays
 *   - an embedded FSB5 blob : the actual compressed samples (their names
 *                     match the manifest filenames, e.g. "char_bad_...ogg")
 * The .strings.bank file carries a STDT chunk: a packed radix trie mapping
 * Event GUIDs to string paths (event:/..., bus:/..., vca:/..., snapshot:/...).
 *
 * The event -> sample (-name) association is NOT present anywhere inside the
 * bank files: events only point at waves by a 16-byte wave GUID, there is no
 * WBAV/wave-name table, the FSB5 blob carries no per-sample GUIDs, and the
 * referenced samples may live in other banks entirely.  That mapping therefore
 * remains supplied by the embedded events_db.bin (built from the FMOD Studio
 * project manifests).  See docs/bank_parsing.md ("Hybrid Approach").  This
 * module parses everything the banks *do* contain so the loader can identify a
 * bank, enumerate its events, and resolve GUID<->path natively.
 *
 * All returned pointers reference borrowed storage (the caller-owned buffer,
 * or the owning BANK's fev struct); the parser allocates only the result
 * arrays, which the caller releases via fev_bank_free().
 */

/* 16-byte FMOD GUID (little-endian, identical layout to FMOD_GUID). */
struct fev_guid {
	uint8_t data[16];
};

/* One event discovered in a bank's EVTS list. */
struct fev_event {
	struct fev_guid guid;       /* event GUID */
	uint32_t n_waves;           /* number of wave references */
	struct fev_guid *waves;     /* wave GUIDs (caller frees), may be NULL */
};

/* Result of parsing a bank's static structure. */
struct fev_string_entry {
	struct fev_guid guid;
	char *path;
};

struct fev_bank {
	const uint8_t *data;        /* bank bytes (borrowed) */
	size_t size;
	struct fev_guid guid;       /* bank GUID from BNKI */
	bool has_guid;
	uint32_t n_events;
	struct fev_event *events;   /* caller frees (and each ->waves) */
	uint32_t n_strings;
	struct fev_string_entry *strings; /* caller frees strings array and each ->path */
};

/* One global GUID -> path mapping gathered from every .strings.bank that
 * has been loaded.  Used to answer FMOD Studio GUID<->path queries natively
 * (no events_db dependency).  See docs/bank_parsing.md ("Hybrid Approach"). */
struct fev_strmap_entry {
	struct fev_guid guid;
	const char *path;       /* borrowed from the owning BANK's fev.strings */
};

struct fev_strmap {
	struct fev_strmap_entry *entries;
	uint32_t count;
	uint32_t cap;
};

int fev_parse(const uint8_t *data, size_t size, struct fev_bank *out);

/* Parse the STDT (String Data Table) from a .strings.bank file.
 * Populates out->strings[] with GUID-to-path mappings extracted from the
 * packed radix trie.  Returns 0 on success, <0 on error.  If no STDT chunk
 * is found, out->n_strings remains 0 and this is not an error. */
int fev_parse_strings(const uint8_t *data, size_t size, struct fev_bank *out);

/* Release storage allocated by fev_parse() / fev_parse_strings(). */
void fev_bank_free(struct fev_bank *bank);

/* Render a GUID as the canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * string into buf (must be >= 37 bytes).  Returns buf. */
char *fev_guid_str(const struct fev_guid *g, char *buf, size_t bufsz);

/* Returns nonzero if two GUIDs are equal. */
int fev_guid_eq(const struct fev_guid *a, const struct fev_guid *b);

/* The process-global GUID->path registry, populated as .strings.bank files
 * are loaded via fev_strmap_add_bank().  Read it through fev_strmap_lookup_*. */
struct fev_strmap *fev_strmap_get(void);

/* Merge a bank's parsed STDT strings into the global registry.  The bank
 * (and its fev.strings) is expected to stay alive for the program's life;
 * the registry only borrows the path pointers.  Safe to call repeatedly;
 * duplicate GUIDs are ignored (first entry wins).  Returns 0 on success. */
int fev_strmap_add_bank(struct fev_bank *bank);

/* Look up a path by GUID.  Returns the path (borrowed) or NULL if unknown. */
const char *fev_strmap_lookup_path(const struct fev_guid *guid);

/* Look up a GUID by its canonical "xxxx-..." string.  Returns nonzero and
 * fills *out on success, 0 otherwise. */
int fev_strmap_lookup_guid(const char *guid_str, struct fev_guid *out);

#endif
