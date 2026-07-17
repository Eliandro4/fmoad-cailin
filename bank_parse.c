#define _GNU_SOURCE
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
#define FEV_STDT	0x54445453u	/* "STDT" (string data table) */
#define FEV_TLNS	0x534e4c54u	/* "TLNS" (wave GUID -> event GUID mappings) */
#define FEV_TLNB	0x424e4c54u	/* "TLNB" (TLNS entry payload) */

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
	 * from TLNS + .strings.bank + FSB5 name matching). */
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
	for (uint32_t i = 0; i < bank->n_strings; i++)
		free(bank->strings[i].path);
	free(bank->strings);
	bank->strings = NULL;
	bank->n_strings = 0;
}

/* ------------------------------------------------------------------ */
/* STDT (String Data Table) parser – packed radix trie.               */
/* ------------------------------------------------------------------ */

static uint32_t rd_u24(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Read an FMOD "x16" variable-length integer (16 or 30 bits). */
static uint32_t read_x16(const uint8_t *data, size_t *off, size_t size)
{
	if (*off + 2 > size) return 0;
	uint32_t val = (uint32_t)data[*off] | ((uint32_t)data[*off + 1] << 8);
	*off += 2;
	if (val & 0x8000) {
		if (*off + 2 > size) return 0;
		uint32_t hi = (uint32_t)data[*off] | ((uint32_t)data[*off + 1] << 8);
		*off += 2;
		val = (val & 0x7FFF) | (hi << 15);
	}
	return val;
}

/* Parse the STDT payload into out->strings[].
 * The STDT format (reverse-engineered from fmodstudioL.ll):
 *   u32   table_type          (0 = 32-bit indices, 1 = 24-bit indices)
 *   FixElemList<PackedNode>   trie nodes  (each 8 bytes: key_offset|key_prefix, flags)
 *   FixElemList<GUID>         value GUIDs (16 bytes each)
 *   byte[]                    string blob (null-terminated segments)
 *   index[]                   guid_indices  (maps GUID ordinal -> node index)
 *   index[]                   parent_indices (maps node index -> parent node) */
static int parse_stdt_payload(const uint8_t *payload, size_t size,
                              struct fev_bank *out)
{
	size_t off = 0;
	if (size < 4) return -1;
	uint32_t table_type = rd_u32(payload);
	off = 4;

	/* --- Nodes (FixElemList) --- */
	uint32_t count_raw_n = read_x16(payload, &off, size);
	uint32_t count_n = count_raw_n >> 1;
	bool is_fixed_n = (count_raw_n & 1) != 0;
	if (count_n == 0) return 0;		/* empty table */

	uint32_t *node_kos = calloc(count_n, sizeof(uint32_t));
	if (!node_kos) return -1;

	uint32_t elem_size = 0;
	for (uint32_t i = 0; i < count_n; i++) {
		if (i == 0 || !is_fixed_n) {
			if (off + 2 > size) goto err_n;
			elem_size = (uint32_t)payload[off] |
			            ((uint32_t)payload[off + 1] << 8);
			off += 2;
		}
		if (off + 8 > size) goto err_n;
		node_kos[i] = rd_u32(payload + off) & 0xFFFFFF;
		off += elem_size;
	}

	/* --- GUIDs (FixElemList) --- */
	uint32_t count_raw_g = read_x16(payload, &off, size);
	uint32_t count_g = count_raw_g >> 1;
	bool is_fixed_g = (count_raw_g & 1) != 0;

	struct fev_guid *guids = calloc(count_g ? count_g : 1,
	                                sizeof(struct fev_guid));
	if (!guids) goto err_n;

	for (uint32_t i = 0; i < count_g; i++) {
		if (i == 0 || !is_fixed_g) {
			if (off + 2 > size) goto err_g;
			elem_size = (uint32_t)payload[off] |
			            ((uint32_t)payload[off + 1] << 8);
			off += 2;
		}
		if (off + 16 > size) goto err_g;
		memcpy(guids[i].data, payload + off, 16);
		off += elem_size;
	}

	/* --- String blob --- */
	uint32_t str_len = read_x16(payload, &off, size);
	if (off + str_len > size) goto err_g;
	const char *str_blob = (const char *)(payload + off);
	off += str_len;

	/* --- Index arrays --- */
	uint32_t *guid_indices = calloc(count_g ? count_g : 1, sizeof(uint32_t));
	uint32_t *parent_indices = calloc(count_n, sizeof(uint32_t));
	if (!guid_indices || !parent_indices) goto err_idx;

	if (table_type == 1) {
		/* 24-bit indices */
		uint32_t cnt_gi = read_x16(payload, &off, size);
		if (cnt_gi > count_g) cnt_gi = count_g;
		for (uint32_t i = 0; i < cnt_gi; i++) {
			if (off + 3 > size) goto err_idx;
			guid_indices[i] = rd_u24(payload + off);
			off += 3;
		}
		uint32_t cnt_pi = read_x16(payload, &off, size);
		if (cnt_pi > count_n) cnt_pi = count_n;
		for (uint32_t i = 0; i < cnt_pi; i++) {
			if (off + 3 > size) goto err_idx;
			parent_indices[i] = rd_u24(payload + off);
			off += 3;
		}
	} else {
		/* 32-bit indices */
		uint32_t cnt_gi = read_x16(payload, &off, size);
		if (cnt_gi > count_g) cnt_gi = count_g;
		for (uint32_t i = 0; i < cnt_gi; i++) {
			if (off + 4 > size) goto err_idx;
			guid_indices[i] = rd_u32(payload + off);
			off += 4;
		}
		uint32_t cnt_pi = read_x16(payload, &off, size);
		if (cnt_pi > count_n) cnt_pi = count_n;
		for (uint32_t i = 0; i < cnt_pi; i++) {
			if (off + 4 > size) goto err_idx;
			parent_indices[i] = rd_u32(payload + off);
			off += 4;
		}
	}

	/* --- Reconstruct paths by walking parent pointers --- */
	out->n_strings = count_g;
	out->strings = calloc(count_g ? count_g : 1,
	                      sizeof(struct fev_string_entry));
	if (!out->strings) goto err_idx;

	uint32_t sentinel = (table_type == 1) ? 0xFFFFFFu : 0xFFFFFFFFu;

	for (uint32_t gi = 0; gi < count_g; gi++) {
		out->strings[gi].guid = guids[gi];

		uint32_t node_idx = guid_indices[gi];
		char path_buf[4096];
		int path_len = 0;
		path_buf[0] = '\0';

		/* Walk from leaf to root, prepending each segment. */
		while (node_idx != sentinel && node_idx < count_n) {
			uint32_t ko = node_kos[node_idx];
			if (ko != 0xFFFFFF && ko < str_len) {
				const char *part = str_blob + ko;
				size_t plen = strnlen(part, str_len - ko);

				if (path_len + (int)plen < (int)sizeof(path_buf)) {
					memmove(path_buf + plen, path_buf,
					        (size_t)path_len + 1);
					memcpy(path_buf, part, plen);
					path_len += (int)plen;
				}
			}
			node_idx = parent_indices[node_idx];
		}
		out->strings[gi].path = strdup(path_buf);
	}

	free(guid_indices);
	free(parent_indices);
	free(guids);
	free(node_kos);
	return 0;

err_idx:
	free(guid_indices);
	free(parent_indices);
err_g:
	free(guids);
err_n:
	free(node_kos);
	return -1;
}

int fev_parse_strings(const uint8_t *data, size_t size, struct fev_bank *out)
{
	if (size < 12)
		return -1;
	if (rd_u32(data) != FEV_RIFF)
		return -1;

	/* Scan for the STDT chunk inside the RIFF container.
	 * .strings.bank files are small, so a linear scan is fine. */
	for (size_t i = 12; i + 8 <= size; ) {
		uint32_t tag = rd_u32(data + i);
		uint32_t sz  = rd_u32(data + i + 4);
		if (tag == FEV_STDT && sz > 0) {
			if (i + 8 + sz > size)
				return -1;
			return parse_stdt_payload(data + i + 8, sz, out);
		}
		/* Skip LIST/RIFF sub-type word, or advance past chunk. */
		if (tag == FEV_RIFF || tag == FEV_LIST)
			i += 12;
		else
			i += 8 + (size_t)sz;
	}
	return 0;	/* no STDT found – not an error */
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

int fev_guid_eq(const struct fev_guid *a, const struct fev_guid *b)
{
	if (!a || !b)
		return 0;
	return memcmp(a->data, b->data, sizeof(a->data)) == 0;
}

/* ------------------------------------------------------------------ */
/* Global GUID -> path registry (fed by parsed .strings.bank files).  */
/* ------------------------------------------------------------------ */

#define STRMAP_INIT_CAP	256

static struct fev_strmap g_strmap = { NULL, 0, 0 };

struct fev_strmap *fev_strmap_get(void)
{
	return &g_strmap;
}

int fev_strmap_add_bank(struct fev_bank *bank)
{
	if (!bank || bank->n_strings == 0)
		return 0;

	if (g_strmap.cap == 0) {
		g_strmap.entries = calloc(STRMAP_INIT_CAP,
		                          sizeof(*g_strmap.entries));
		if (!g_strmap.entries)
			return -1;
		g_strmap.cap = STRMAP_INIT_CAP;
	}

	for (uint32_t i = 0; i < bank->n_strings; i++) {
		if (!bank->strings[i].path)
			continue;

		/* Skip duplicates (first bank to define a GUID wins). */
		bool dup = false;
		for (uint32_t j = 0; j < g_strmap.count; j++) {
			if (fev_guid_eq(&g_strmap.entries[j].guid,
			                &bank->strings[i].guid)) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		if (g_strmap.count == g_strmap.cap) {
			uint32_t ncap = g_strmap.cap * 2;
			struct fev_strmap_entry *ne = realloc(g_strmap.entries,
				ncap * sizeof(*ne));
			if (!ne)
				return -1;
			g_strmap.entries = ne;
			g_strmap.cap = ncap;
		}
		g_strmap.entries[g_strmap.count].guid = bank->strings[i].guid;
		g_strmap.entries[g_strmap.count].path = bank->strings[i].path;
		g_strmap.count++;
	}
	return 0;
}

const char *fev_strmap_lookup_path(const struct fev_guid *guid)
{
	if (!guid)
		return NULL;
	for (uint32_t i = 0; i < g_strmap.count; i++)
		if (fev_guid_eq(&g_strmap.entries[i].guid, guid))
			return g_strmap.entries[i].path;
	return NULL;
}

int fev_strmap_lookup_guid(const char *guid_str, struct fev_guid *out)
{
	if (!guid_str || !out)
		return 0;
	struct fev_guid g;
	memset(&g, 0, sizeof(g));
	/* Accept the canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" form. */
	int n = 0;
	for (int i = 0; i < 16; i++) {
		/* Skip the three separator dashes. */
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			if (guid_str[n] != '-')
				return 0;
			n++;
		}
		unsigned hi;
		if (sscanf(guid_str + n, "%2x", &hi) != 1)
			return 0;
		g.data[i] = (uint8_t)hi;
		n += 2;
	}
	*out = g;
	return 1;
}

/* ------------------------------------------------------------------ */
/* TLNS (wave GUID -> event GUID mapping) parser                     */
/* ------------------------------------------------------------------ */

int fev_parse_tlns(const uint8_t *data, size_t size, struct fev_tlns *out)
{
	if (!data || !out || size < 12)
		return -1;

	memset(out, 0, sizeof(*out));

	/* Find the TLNS LIST chunk.  In this FEV flavour the TLNS section is a
	 * tagged marker followed by LCNT and child LIST entries, each wrapping
	 * a TLNB payload that holds one wave GUID and one event GUID. */
	const uint8_t *tlns_marker = NULL;
	for (size_t i = 12; i + 4 <= size; i++) {
		if (rd_u32(data + i) == FEV_TLNS) {
			tlns_marker = data + i;
			break;
		}
	}
	if (!tlns_marker)
		return 0;

	/* Bound the scan at EOF. */
	const uint8_t *end = data + size;

	/* Parse child LIST entries under TLNS.  Each child is:
	 *   LIST TMLN
	 *     TLNB magic(4) + unknown(4) + wave_guid(16) + event_guid(16) + ... */
	const uint8_t *p = tlns_marker + 4; /* skip TLNS tag */
	while (p + 8 <= end) {
		uint32_t tag = rd_u32(p);
		uint32_t sz = rd_u32(p + 4);
		const uint8_t *cend = p + 8 + (size_t)sz;
		if (cend > end)
			break;

		if (tag == FEV_LIST && cend - p >= 12) {
			const uint8_t *payload = p + 12;

			/* Look for TLNB inside the LIST payload. */
			const uint8_t *tlnb = payload;
			while (tlnb + 8 <= cend) {
				if (rd_u32(tlnb) == FEV_TLNB) {
					uint32_t tlnb_size = rd_u32(tlnb + 4);
					const uint8_t *tlnb_payload = tlnb + 8;
					const uint8_t *tlnb_end = tlnb + 8 + (size_t)tlnb_size;
					if (tlnb_end > cend)
						break;

					/* Need at least: wave(16) + event(16) */
					if (tlnb_end - tlnb_payload >= 32) {
						if (out->count == out->cap) {
							uint32_t ncap = out->cap ? out->cap * 2 : 16;
							struct fev_tlns_entry *ne = realloc(out->entries,
							                                     ncap * sizeof(*ne));
							if (!ne) {
								free(out->entries);
								out->entries = NULL;
								return -1;
							}
							out->entries = ne;
							out->cap = ncap;
						}
						struct fev_tlns_entry *e = &out->entries[out->count];
						memcpy(e->wave_guid.data, tlnb_payload, 16);
						memcpy(e->event_guid.data, tlnb_payload + 16, 16);
						out->count++;
					}
					break;
				}
				tlnb += 8;
			}
		}

		size_t step = (size_t)sz + 8;
		if (step < 8 || p + step > end)
			break;
		p += step;
	}

	return 0;
}

void fev_tlns_free(struct fev_tlns *tlns)
{
	if (!tlns)
		return;
	free(tlns->entries);
	tlns->entries = NULL;
	tlns->count = 0;
	tlns->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Heuristic: event path -> candidate sample name(s)                  */
/* ------------------------------------------------------------------ */

char *fev_event_path_to_sample_name(const char *event_path)
{
	if (!event_path)
		return NULL;

	/* Remove 'event:/' prefix */
	const char *p = event_path;
	if (strncmp(p, "event:/", 7) == 0)
		p += 7;

	/* Split by '/' and clean each part */
	char buf[4096];
	int blen = 0;
	buf[0] = '\0';

	const char *part = p;
	while (*part) {
		const char *slash = strchr(part, '/');
		size_t plen = slash ? (size_t)(slash - part) : strlen(part);

		/* Copy part */
		char segment[256];
		if (plen >= sizeof(segment))
			plen = sizeof(segment) - 1;
		memcpy(segment, part, plen);
		segment[plen] = '\0';

		/* Remove common subfolder suffixes */
		char *cleaned = segment;
		const char *suffixes[] = {
			"_map", "_cliffside", "_temple", "_reflection",
			"_forsaken_city", "_summit", "_ridge", "_heaven", NULL
		};
		for (int i = 0; suffixes[i]; i++) {
			size_t slen = strlen(suffixes[i]);
			if (plen > slen && memcmp(cleaned + plen - slen, suffixes[i], slen) == 0) {
				cleaned[plen - slen] = '\0';
				break;
			}
		}

		/* Append to buffer */
		if (blen > 0)
			buf[blen++] = '_';
		size_t clen = strlen(cleaned);
		if (blen + clen < sizeof(buf) - 1) {
			memcpy(buf + blen, cleaned, clen);
			blen += clen;
		}
		buf[blen] = '\0';

		if (!slash)
			break;
		part = slash + 1;
	}

	/* Pad numbers followed by 'ms' to 4 digits */
	char out[4096];
	char *dst = out;
	const char *src = buf;
	while (*src) {
		if (*src >= '0' && *src <= '9') {
			const char *num_start = src;
			while (*src >= '0' && *src <= '9')
				src++;
			size_t num_len = (size_t)(src - num_start);
			if (strncmp(src, "ms", 2) == 0 && num_len < 4) {
				for (size_t i = 0; i < 4 - num_len; i++)
					*dst++ = '0';
			}
			memcpy(dst, num_start, num_len);
			dst += num_len;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';

	return strdup(out);
}

/* Match a candidate sample name against the FSB5 sample names.
 * Returns a newly allocated array of matching sample names (caller frees). */
char **fev_match_sample_names(const char *candidate, const char **sample_names,
                          uint32_t sample_count, uint32_t *out_count)
{
	if (!candidate || !sample_names || sample_count == 0) {
		*out_count = 0;
		return NULL;
	}

	uint32_t cap = 4;
	char **matches = calloc(cap, sizeof(char *));
	if (!matches) {
		*out_count = 0;
		return NULL;
	}

	size_t clen = strlen(candidate);
	for (uint32_t i = 0; i < sample_count; i++) {
		const char *name = sample_names[i];
		size_t nlen = strlen(name);

		/* Exact match */
		if (strcmp(name, candidate) == 0) {
			if (*out_count == cap) {
				cap *= 2;
				char **nm = realloc(matches, cap * sizeof(char *));
				if (!nm) {
					for (uint32_t j = 0; j < *out_count; j++)
						free(matches[j]);
					free(matches);
					*out_count = 0;
					return NULL;
				}
				matches = nm;
			}
			matches[*out_count] = strdup(name);
			(*out_count)++;
			continue;
		}

		/* Prefix match: candidate + '_' + digits */
		if (nlen > clen && memcmp(name, candidate, clen) == 0 &&
		    name[clen] == '_') {
			const char *suffix = name + clen + 1;
			int all_digits = 1;
			for (const char *c = suffix; *c; c++) {
				if (*c < '0' || *c > '9') {
					all_digits = 0;
					break;
				}
			}
			if (all_digits) {
				if (*out_count == cap) {
					cap *= 2;
					char **nm = realloc(matches, cap * sizeof(char *));
					if (!nm) {
						for (uint32_t j = 0; j < *out_count; j++)
							free(matches[j]);
						free(matches);
						*out_count = 0;
						return NULL;
					}
					matches = nm;
				}
				matches[*out_count] = strdup(name);
				(*out_count)++;
			}
		}
	}

	if (*out_count == 0) {
		free(matches);
		matches = NULL;
	}

	return matches;
}

/* ------------------------------------------------------------------ */
/* Native event->sample mapping builder.                              */
/* ------------------------------------------------------------------ */

int bank_build_events(const char *bankname, const uint8_t *bank_data,
                      size_t bank_size, const char **sample_names,
                      uint32_t sample_count, struct bank_events *out)
{
	if (!bankname || !bank_data || !sample_names || !out)
		return 0;

	memset(out, 0, sizeof(*out));
	out->bankname = bankname;

	/* Parse TLNS to get wave_guid -> event_guid mappings. */
	struct fev_tlns tlns;
	if (fev_parse_tlns(bank_data, bank_size, &tlns) < 0)
		return 0;

	if (tlns.count == 0) {
		fev_tlns_free(&tlns);
		return 0;
	}

	/* Look up event paths from the global string map. */
	struct fev_strmap *strmap = fev_strmap_get();

	/* First pass: count entries.  We add one entry per (event, sample) pair.
	 * For intra-bank samples, the sample is matched from the local FSB5 names
	 * using the event-path heuristic.  For cross-bank samples, the wave GUID
	 * is looked up in the global wavemap. */
	uint32_t total_entries = 0;
	for (uint32_t i = 0; i < tlns.count; i++) {
		const char *path = NULL;
		if (strmap && strmap->count > 0)
			path = fev_strmap_lookup_path(&tlns.entries[i].event_guid);

		if (path) {
			char *candidate = fev_event_path_to_sample_name(path);
			if (candidate) {
				uint32_t matches = 0;
				char **sample_matches = fev_match_sample_names(candidate,
				                                              sample_names,
				                                              sample_count,
				                                              &matches);
				free(candidate);
				total_entries += matches;
				free(sample_matches);
			}
		}

		/* Cross-bank fallback: if no local match, check global wavemap. */
		if (total_entries == 0 || path == NULL) {
			const char *xbank_sample = fev_wavemap_lookup(&tlns.entries[i].wave_guid);
			if (xbank_sample)
				total_entries++;
		}
	}

	if (total_entries == 0) {
		fev_tlns_free(&tlns);
		return 0;
	}

	/* Allocate and populate the entry array. */
	struct event_entry *entries = calloc(total_entries, sizeof(*entries));
	if (!entries) {
		fev_tlns_free(&tlns);
		return 0;
	}

	uint32_t idx = 0;
	for (uint32_t i = 0; i < tlns.count && idx < total_entries; i++) {
		const char *path = NULL;
		if (strmap && strmap->count > 0)
			path = fev_strmap_lookup_path(&tlns.entries[i].event_guid);

		if (path) {
			char *candidate = fev_event_path_to_sample_name(path);
			if (candidate) {
				uint32_t matches = 0;
				char **sample_matches = fev_match_sample_names(candidate,
				                                              sample_names,
				                                              sample_count,
				                                              &matches);
				free(candidate);

				for (uint32_t j = 0; j < matches && idx < total_entries; j++) {
					entries[idx].path = strdup(path);
					entries[idx].file = sample_matches[j];
					idx++;
				}
				free(sample_matches);
				continue;
			}
		}

		/* Cross-bank fallback. */
		const char *xbank_sample = fev_wavemap_lookup(&tlns.entries[i].wave_guid);
		if (xbank_sample && idx < total_entries) {
			entries[idx].path = path ? strdup(path) : strdup("unknown");
			entries[idx].file = xbank_sample;
			idx++;
		}
	}

	fev_tlns_free(&tlns);
	out->count = idx;
	out->entries = entries;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Global wave GUID -> sample name registry (cross-bank resolution).  */
/* ------------------------------------------------------------------ */

static struct fev_wavemap g_wavemap = { NULL, 0, 0 };

void fev_wavemap_add_bank(const char *bankname, const uint8_t *bank_data,
                          size_t bank_size, const char **sample_names,
                          uint32_t sample_count)
{
	if (!bankname || !bank_data || !sample_names || sample_count == 0)
		return;

	/* Parse TLNS to get wave_guid -> event_guid mappings. */
	struct fev_tlns tlns;
	if (fev_parse_tlns(bank_data, bank_size, &tlns) < 0)
		return;

	if (tlns.count == 0) {
		fev_tlns_free(&tlns);
		return;
	}

	/* Collect unique event GUIDs from TLNS. */
	uint32_t cap = tlns.count;
	struct fev_guid *event_guids = calloc(cap, sizeof(*event_guids));
	if (!event_guids) {
		fev_tlns_free(&tlns);
		return;
	}
	uint32_t nevents = 0;
	for (uint32_t i = 0; i < tlns.count; i++) {
		const struct fev_guid *eg = &tlns.entries[i].event_guid;
		bool found = false;
		for (uint32_t j = 0; j < nevents; j++) {
			if (fev_guid_eq(eg, &event_guids[j])) {
				found = true;
				break;
			}
		}
		if (!found && nevents < cap) {
			event_guids[nevents++] = *eg;
		}
	}
	fev_tlns_free(&tlns);

	if (nevents == 0) {
		free(event_guids);
		return;
	}

	/* For each event, resolve to sample name and register wave_guid -> sample_name. */
	for (uint32_t i = 0; i < nevents; i++) {
		const char *path = fev_strmap_lookup_path(&event_guids[i]);
		if (!path)
			continue;

		char *candidate = fev_event_path_to_sample_name(path);
		if (!candidate)
			continue;

		uint32_t matches = 0;
		char **sample_matches = fev_match_sample_names(candidate, sample_names,
		                                              sample_count, &matches);
		free(candidate);

		for (uint32_t j = 0; j < matches; j++) {
			/* Find all wave GUIDs for this event in TLNS. */
			for (uint32_t k = 0; k < tlns.count; k++) {
				if (fev_guid_eq(&tlns.entries[k].event_guid, &event_guids[i])) {
					/* Add to global wavemap. */
					if (g_wavemap.count == g_wavemap.cap) {
						uint32_t ncap = g_wavemap.cap ? g_wavemap.cap * 2 : 64;
						struct fev_wave_entry *ne = realloc(g_wavemap.entries,
						                                     ncap * sizeof(*ne));
						if (!ne) {
							free(sample_matches);
							free(event_guids);
							fev_tlns_free(&tlns);
							return;
						}
						g_wavemap.entries = ne;
						g_wavemap.cap = ncap;
					}
					struct fev_wave_entry *we = &g_wavemap.entries[g_wavemap.count];
					we->wave_guid = tlns.entries[k].wave_guid;
					we->sample_name = strdup(sample_matches[j]);
					g_wavemap.count++;
				}
			}
			free(sample_matches[j]);
		}
		free(sample_matches);
	}

	free(event_guids);
	fev_tlns_free(&tlns);
}

const char *fev_wavemap_lookup(const struct fev_guid *wave_guid)
{
	if (!wave_guid || !g_wavemap.entries)
		return NULL;

	for (uint32_t i = 0; i < g_wavemap.count; i++) {
		if (fev_guid_eq(wave_guid, &g_wavemap.entries[i].wave_guid))
			return g_wavemap.entries[i].sample_name;
	}
	return NULL;
}

void fev_wavemap_clear(void)
{
	for (uint32_t i = 0; i < g_wavemap.count; i++)
		free(g_wavemap.entries[i].sample_name);
	free(g_wavemap.entries);
	g_wavemap.entries = NULL;
	g_wavemap.count = 0;
	g_wavemap.cap = 0;
}

struct fev_wavemap *fev_wavemap_get(void)
{
	return &g_wavemap;
}

void events_db_free(struct bank_events *be)
{
	if (!be || !be->entries)
		return;
	for (uint32_t i = 0; i < be->count; i++) {
		free((void *)be->entries[i].path);
		free((void *)be->entries[i].file);
	}
	free(be->entries);
	be->entries = NULL;
	be->count = 0;
}
