#ifndef BANK_PARSE_H
#define BANK_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Native FMOD FEV bank parser.
 *
 * Celeste ships its audio as FMOD "FEV" banks (RIFF container with a FEV
 * 'FMT ' chunk), not the newer FMOD Studio format.  Each .bank file carries:
 *   - a BNKI chunk  : the bank GUID (and a 16-byte reserved block)
 *   - an EVTS list  : one EVNT entry per event, each with a 16-byte event
 *                     GUID and references to the waves it plays
 *   - an embedded FSB5 blob : the actual compressed samples (their names
 *                     match the manifest filenames, e.g. "char_bad_...ogg")
 *
 * The event -> sample (-name) association is NOT present anywhere inside the
 * bank files: events only point at waves by a 16-byte wave GUID, there is no
 * WBAV/wave-name table, the FSB5 blob carries no per-sample GUIDs, and the
 * referenced samples may live in other banks entirely.  That mapping therefore
 * remains supplied by the embedded events_db.bin (built from the FMOD Studio
 * project manifests).  This module parses everything the banks *do* contain so
 * the loader no longer depends on external JSON manifests or the filename to
 * identify a bank, and can enumerate a bank's events natively.
 *
 * All returned pointers reference the caller-owned buffer; the parser performs
 * no allocation of its own (beyond the caller's result struct).
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
struct fev_bank {
	const uint8_t *data;        /* bank bytes (borrowed) */
	size_t size;
	struct fev_guid guid;       /* bank GUID from BNKI */
	bool has_guid;
	uint32_t n_events;
	struct fev_event *events;   /* caller frees (and each ->waves) */
};

/* Parse the FEV container and enumerate its events.
 * Returns 0 on success (even if the bank has zero events), <0 on a fatal
 * parse error (not a RIFF/FEV file, truncated, ...).  On success the caller
 * owns ->events and each ->waves and must release them with fev_bank_free(). */
int fev_parse(const uint8_t *data, size_t size, struct fev_bank *out);

/* Release storage allocated by fev_parse(). */
void fev_bank_free(struct fev_bank *bank);

/* Render a GUID as the canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * string into buf (must be >= 37 bytes).  Returns buf. */
char *fev_guid_str(const struct fev_guid *g, char *buf, size_t bufsz);

#endif
