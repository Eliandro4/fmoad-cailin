#ifndef BANK_PARSE_H
#define BANK_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Native FMOD Studio bank parser with cross-bank wavemap.
 *
 * Each .bank file (RIFF container) carries:
 *   - a BNKI chunk  : the bank GUID (and a 16-byte reserved block)
 *   - an EVTS list  : one EVNT entry per event, each with a 16-byte event
 *                     GUID and references to the waves it plays
 *   - an embedded FSB5 blob : the actual compressed samples (their names
 *                     match the manifest filenames, e.g. "char_bad_...ogg")
 *   - a TLNS section: maps each wave GUID to the event GUID that references it
 *   - a SNDH chunk  : lists the embedded FSB5 blob offsets/sizes
 * The .strings.bank file carries a STDT chunk: a packed radix trie mapping
 * Event GUIDs to string paths (event:/..., bus:/..., vca:/..., snapshot:/...).
 *
 * Event -> sample resolution is performed natively:
 *   1. TLNS gives wave_guid -> event_guid
 *   2. .strings.bank gives event_guid -> event_path
 *   3. Heuristic converts event_path -> candidate sample_name
 *   4. FSB5 sample names are matched against the candidate
 * Cross-bank references are resolved via the global wave registry
 * populated as banks are loaded.
 *
 * All returned pointers reference borrowed storage (the caller-owned buffer,
 * or the owning BANK's fev struct); the parser allocates only the result
 * arrays, which the caller releases via events_db_free().
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

/* Event -> sample mapping entry. */
struct event_entry {
	const char *path;   /* event:/... path (lookup key) */
	const char *file;   /* sample filename (== FSB5 sample name) */
};

struct bank_events {
	const char *bankname;          /* base name, e.g. "sfx" */
	uint32_t count;
	struct event_entry *entries;   /* caller frees */
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

/* One wave GUID -> event GUID mapping extracted from the TLNS section. */
struct fev_tlns_entry {
	struct fev_guid wave_guid;
	struct fev_guid event_guid;
};

/* Result of parsing a bank's TLNS section. */
struct fev_tlns {
	struct fev_tlns_entry *entries;
	uint32_t count;
	uint32_t cap;
};

/* Global wave GUID -> sample name registry, populated as banks are loaded.
 * Used for cross-bank sample resolution. */
struct fev_wave_entry {
	struct fev_guid wave_guid;
	char *sample_name;      /* heap-allocated */
};

struct fev_wavemap {
	struct fev_wave_entry *entries;
	uint32_t count;
	uint32_t cap;
};

int fev_parse(const uint8_t *data, size_t size, struct fev_bank *out);

/* Parse the TLNS section from a bank file.  Populates out with wave GUID to
 * event GUID mappings.  Returns 0 on success, <0 on error.  If no TLNS
 * section is found, out->count remains 0 and this is not an error. */
int fev_parse_tlns(const uint8_t *data, size_t size, struct fev_tlns *out);

/* Release storage allocated by fev_parse_tlns(). */
void fev_tlns_free(struct fev_tlns *tlns);

/* Heuristic: convert an FMOD Studio event path to a candidate sample name.
 * The returned string must be freed by the caller. */
char *fev_event_path_to_sample_name(const char *event_path);

/* Match a candidate sample name against FSB5 sample names.
 * Returns a newly allocated array of matching names (caller frees each
 * string and the array itself).  *out_count receives the number of matches. */
char **fev_match_sample_names(const char *candidate, const char **sample_names,
                               uint32_t sample_count, uint32_t *out_count);

/* Build the bank->events mapping natively from TLNS + .strings.bank + FSB5.
 * Returns 1 on success (out is populated), 0 if the bank has no events or
 * the native mapping could not be built.  The caller frees out->entries
 * and each entry's path with events_db_free(). */
int bank_build_events(const char *bankname, const uint8_t *bank_data,
                      size_t bank_size, const char **sample_names,
                      uint32_t sample_count, struct bank_events *out);

/* Free the entry array allocated by bank_build_events(). */
void events_db_free(struct bank_events *be);

/* Register all samples from a loaded bank into the global wave registry.
 * This enables cross-bank sample resolution: when bank A's events reference
 * waves that live in bank B, the lookup will find them here. */
void fev_wavemap_add_bank(const char *bankname, const uint8_t *bank_data,
                          size_t bank_size, const char **sample_names,
                          uint32_t sample_count);

/* Look up a wave GUID in the global registry.  Returns the sample name
 * (borrowed) or NULL if not found. */
const char *fev_wavemap_lookup(const struct fev_guid *wave_guid);

/* Clear the global wave registry. */
void fev_wavemap_clear(void);

/* Access the global wave registry (for testing/inspection). */
struct fev_wavemap *fev_wavemap_get(void);

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
