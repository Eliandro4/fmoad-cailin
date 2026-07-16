/*
 * Native FMOD FEV bank parser.  See bank_parse.h for the format notes and the
 * public API.  The parser walks the RIFF/LIST container hierarchy and pulls the
 * bank GUID out of BNKI and the per-event GUID out of EVBT (inside EVTS).
 *
 * No allocation is performed except for the result arrays (fev_event / wave
 * GUID lists), which the caller releases via fev_bank_free().
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "bank_parse.h"

#define FEV_RIFF	0x46464952u	/* "RIFF" */
#define FEV_FEV		0x20564546u	/* "FEV " */
#define FEV_LIST	0x5453494cu	/* "LIST" */
#define FEV_BNKI	0x494b4e42u	/* "BNKI" */
#define FEV_EVTS	0x53545645u	/* "EVTS" */
#define FEV_EVNT	0x544e5645u	/* "EVNT" (LIST type wrapping an event) */
#define FEV_EVBT	0x42545645u	/* "EVBT" (event body chunk) */

static uint32_t rd_u32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

/* Walk the RIFF/LIST tree under [start, start+len).  For every chunk whose
 * 4-byte tag equals `id`, invoke cb(id, payload, size, uarg).  LIST chunks are
 * descended into (their payload is `type(4)` + children), so nested chunks are
 * all visited.  Returns the number of matching chunks seen. */
typedef void (*chunk_cb)(uint32_t id, const uint8_t *payload,
                         uint32_t size, void *uarg);

static size_t walk_chunks(const uint8_t *start, size_t len,
                          uint32_t id, chunk_cb cb, void *uarg)
{
	const uint8_t *p = start;
	const uint8_t *end = start + len;
	size_t hits = 0;
	while (p + 8 <= end) {
		uint32_t tag = rd_u32(p);
		uint32_t sz = rd_u32(p + 4);
		const uint8_t *payload = p + 8;
		const uint8_t *cend = p + 8 + (size_t)sz;
		if (cend > end || cend < p)
			cend = end;

		if (tag == FEV_LIST) {
			/* LIST payload: type(4) + nested children. */
			if (payload + 4 <= cend &&
			    (size_t)(cend - (payload + 4)) <= len)
				hits += walk_chunks(payload + 4,
				                    (size_t)(cend - (payload + 4)),
				                    id, cb, uarg);
		} else if (tag == id) {
			cb(id, payload, sz, uarg);
			hits++;
		}

		size_t step = (size_t)sz + 8;
		if (step < 8 || p + step > end)
			step = 8;
		p += step;
	}
	return hits;
}

/* Growable event array accumulated while walking the EVBT chunks. */
struct evnt_ctx {
	struct fev_event *arr;
	uint32_t count;
	uint32_t cap;
};

static void on_evnt(uint32_t id, const uint8_t *payload,
                    uint32_t size, void *uarg);

/* Gather the BNKI GUID while walking. */
struct parse_ctx {
	struct fev_bank *out;
};

static void on_chunk(uint32_t id, const uint8_t *payload,
                     uint32_t size, void *uarg)
{
	struct parse_ctx *ctx = uarg;
	if (id == FEV_BNKI && size >= 16 && !ctx->out->has_guid) {
		memcpy(ctx->out->guid.data, payload, 16);
		ctx->out->has_guid = true;
	}
}

int fev_parse(const uint8_t *data, size_t size, struct fev_bank *out)
{
	if (size < 12)
		return -1;
	if (rd_u32(data) != FEV_RIFF)
		return -1;
	if (rd_u32(data + 8) != FEV_FEV)
		return -1;

	memset(out, 0, sizeof(*out));
	out->data = data;
	out->size = size;
	out->has_guid = false;

	struct parse_ctx ctx = { out };

	/* Bank GUID from BNKI (nested anywhere under the RIFF body). */
	walk_chunks(data + 12, size - 12, FEV_BNKI, on_chunk, &ctx);

	/* Enumerate EVNT entries.  In this FEV flavour the EVTS section is a
	 * tagged marker (no enclosing size) followed by `LCNT count` and then,
	 * per category, a `LIST EVNT ... EVBT ...` entry.  The event GUIDs
	 * live in the EVBT chunks; we scan for the "EVBT" tag directly within
	 * the EVTS region (bounded by the embedded FSB5 blob, which contains
	 * no EVBT tags) to avoid descending into the arbitrary bytes of the
	 * audio data.  Each EVBT payload begins with the event GUID (16), a
	 * reserved block (16), then a run of 16-byte wave GUID references. */
	struct evnt_ctx ec = { NULL, 0, 0 };
	const uint8_t *evts_marker = NULL;
	for (size_t i = 12; i + 4 <= size; i++) {
		if (rd_u32(data + i) == FEV_EVTS) {
			evts_marker = data + i;
			break;
		}
	}
	if (evts_marker) {
		/* Bound the scan at the FSB5 blob (or EOF). */
		const uint8_t *fsb = data + size;
		for (size_t i = 12; i + 4 <= size; i++) {
			if (rd_u32(data + i) == 0x35425346u) { /* "FSB5" */
				fsb = data + i;
				break;
			}
		}
		for (const uint8_t *p = evts_marker; p + 8 <= fsb; p++) {
			if (rd_u32(p) == FEV_EVBT)
				on_evnt(FEV_EVBT, p + 8,
				        rd_u32(p + 4), &ec);
		}
	}

	if (ec.count == 0) {
		free(ec.arr);
		return 0;	/* no events in this bank (e.g. a strings bank) */
	}

	out->events = ec.arr;
	out->n_events = ec.count;
	return 0;
}

/* Append one parsed event (from an EVBT chunk payload) to the growable array
 * kept in the evnt_ctx. */
static void on_evnt(uint32_t id, const uint8_t *payload,
                    uint32_t size, void *uarg)
{
	(void)id;
	struct evnt_ctx *ec = uarg;

	if (size < 16 + 16)			/* need guid + reserved */
		return;

	const uint8_t *g = payload;		/* EVBT payload starts with GUID */
	if (ec->count == ec->cap) {
		uint32_t ncap = ec->cap ? ec->cap * 2 : 16;
		struct fev_event *na = realloc(ec->arr, ncap * sizeof(*na));
		if (!na)
			return;
		ec->arr = na;
		ec->cap = ncap;
	}

	struct fev_event *e = &ec->arr[ec->count];
	memset(e, 0, sizeof(*e));
	memcpy(e->guid.data, g, 16);

	/* The remaining bytes are the event body: a reserved 16-byte block,
	 * then a run of 16-byte wave GUID references (and other fixed /
	 * property chunks).  Record each 16-byte run after the reserved block
	 * as a wave reference.  The exact count is approximate; it is used
	 * only for diagnostics, never for sample lookup (that mapping comes
	 * from events_db). */
	const uint8_t *b = g + 16 + 16;
	const uint8_t *end = payload + size;
	uint32_t cap = 8;
	struct fev_guid *wg = NULL;
	uint32_t nw = 0;
	while (b + 16 <= end) {
		if (nw == cap) {
			cap *= 2;
			struct fev_guid *nw2 = realloc(wg, cap * sizeof(*wg));
			if (!nw2) {
				free(wg);
				wg = NULL;
				break;
			}
			wg = nw2;
		}
		memcpy(wg[nw].data, b, 16);
		nw++;
		b += 16;
	}
	e->waves = wg;
	e->n_waves = nw;

	ec->count++;
}

void fev_bank_free(struct fev_bank *bank)
{
	if (!bank)
		return;
	for (uint32_t i = 0; i < bank->n_events; i++)
		free(bank->events[i].waves);
	free(bank->events);
	bank->events = NULL;
	bank->n_events = 0;
}

char *fev_guid_str(const struct fev_guid *g, char *buf, size_t bufsz)
{
	if (!g || !buf || bufsz < 37)
		return buf;
	const uint8_t *d = g->data;
	snprintf(buf, bufsz,
	         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
	         d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
	return buf;
}
