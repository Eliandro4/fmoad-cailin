/*
 * Vorbis -> OGG rebuild for FSB5 samples.
 *
 * Reconstructs a valid OGG Vorbis byte stream from the raw Vorbis packets
 * stored inside an FSB5 blob, using the crc32 -> setup-header database
 * (vorbis_db.bin, embedded via objcopy).  The algorithm follows Fmod5Sharp's
 * FmodVorbisRebuilder and is validated against python-fsb5 (which uses the
 * real libvorbis/libogg).  Bit reading is LSB-first (IMPLEMENTATION.md §12.8).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fsb.h"
#include "vorbis_ogg.h"

/* ------------------------------------------------------------------ */
/* Embedded setup database (vorbis_db.bin)                             */
/* ------------------------------------------------------------------ */
extern unsigned char vorbis_db_bin[];
extern unsigned int vorbis_db_bin_len;

struct db_entry {
    uint32_t crc;
    int32_t  seekbit;
    uint32_t hdrlen;
    const uint8_t *hdr;
};

static uint32_t db_read_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int32_t db_read_i32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* Build a temporary view of the DB: returns entry count, fills *entries
 * (caller provides an array of sufficient size, or NULL to just count). */
static uint32_t db_enumerate(struct db_entry *entries, uint32_t max)
{
    const uint8_t *p = (const uint8_t *)vorbis_db_bin;
    const uint8_t *end = p + vorbis_db_bin_len;
    
    if (p + 4 > end)
        return 0;
    uint32_t count = db_read_u32(p);
    p += 4;
    uint32_t got = 0;
    for (uint32_t i = 0; i < count && p + 12 <= end; i++) {
        uint32_t crc = db_read_u32(p);
        int32_t seekbit = db_read_i32(p + 4);
        uint32_t hdrlen = db_read_u32(p + 8);
        const uint8_t *hdr = p + 12;
        p += 12 + hdrlen;
        if (p > end)
            break;
        if (entries && got < max) {
            entries[got].crc = crc;
            entries[got].seekbit = seekbit;
            entries[got].hdrlen = hdrlen;
            entries[got].hdr = hdr;
        }
        got++;
    }
    return got;
}

int vorbis_db_lookup(uint32_t crc, const uint8_t **hdr,
                     uint32_t *hdrlen, int32_t *seekbit)
{
    uint32_t count = db_enumerate(NULL, 0);
    if (count == 0)
        return 0;
    /* The DB is sorted ascending by crc, so binary search. */
    struct db_entry *entries = malloc(sizeof(*entries) * count);
    if (!entries)
        return 0;
    db_enumerate(entries, count);
    int lo = 0, hi = (int)count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].crc == crc) {
            *hdr = entries[mid].hdr;
            *hdrlen = entries[mid].hdrlen;
            *seekbit = entries[mid].seekbit;
            free(entries);
            return 1;
        } else if (entries[mid].crc < crc) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    free(entries);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LSB-first bit reader (IMPLEMENTATION.md §12.8)                      */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bytepos;
    int bitpos;
} bitr;

static void bitr_init(bitr *b, const uint8_t *data, size_t len)
{
    b->data = data;
    b->len = len;
    b->bytepos = 0;
    b->bitpos = 0;
}

static int bitr_bit(bitr *b)
{
    if (b->bytepos >= b->len)
        return 0;
    int v = (b->data[b->bytepos] >> b->bitpos) & 1;
    b->bitpos++;
    if (b->bitpos == 8) {
        b->bitpos = 0;
        b->bytepos++;
    }
    return v;
}

static uint32_t bitr_bits(bitr *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)bitr_bit(b) << i;   /* LSB first */
    return v;
}

static uint8_t bitr_u8(bitr *b)
{
    return (uint8_t)bitr_bits(b, 8);
}

static void bitr_seekbit(bitr *b, int absbit)
{
    b->bytepos = (size_t)(absbit / 8);
    b->bitpos = absbit % 8;
}

static void bitr_skip(bitr *b, int n)
{
    for (int i = 0; i < n; i++)
        (void)bitr_bit(b);
}

static int ilog(int v)
{
    int n = 0;
    while ((1 << n) < v)
        n++;
    return n;
}

/* Parse blockflag[mode] from the setup packet at the given seek bit. */
static int compute_block_flags(const uint8_t *hdr, uint32_t len,
                               int32_t seekbit, int *blockflag,
                               int *num_modes)
{
    bitr b;
    bitr_init(&b, hdr, len);
    if (bitr_u8(&b) != 5)            /* packing type */
        return -1;
    uint8_t magic[6];
    for (int i = 0; i < 6; i++)
        magic[i] = bitr_u8(&b);
    if (memcmp(magic, "vorbis", 6) != 0)
        return -1;
    bitr_seekbit(&b, seekbit);       /* absolute bit position */
    int nm = (int)bitr_bits(&b, 6) + 1;
    *num_modes = nm;
    for (int i = 0; i < nm; i++) {
        int flag = bitr_bit(&b);
        bitr_skip(&b, 16 + 16 + 8); /* 40 bits per mode */
        if (i < 64)
            blockflag[i] = flag;
    }
    return 0;
}

/* Vorbis packet blocksize (matches libvorbis vorbis_packet_blocksize). */
static int packet_block_size(const uint8_t *pkt, size_t len,
                             int *blockflag, int num_modes)
{
    bitr b;
    bitr_init(&b, pkt, len);
    if (bitr_bit(&b) == 1)           /* not an audio packet */
        return 0;
    int mode = 0;
    if (num_modes > 1)
        mode = (int)bitr_bits(&b, ilog(num_modes - 1));
    return blockflag[mode] ? 2048 : 256;
}

/* ------------------------------------------------------------------ */
/* OGG page writer                                                     */
/* ------------------------------------------------------------------ */
static const uint32_t ogg_crc_table[256] = {
0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
    0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9, 0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75,
    0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3, 0x709F7B7A, 0x745E66CD,
    0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039, 0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5,
    0xBE2B5B58, 0xBAEA46EF, 0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
    0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49, 0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95,
    0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1, 0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D,
    0x34867077, 0x30476DC0, 0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072,
    0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16, 0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA,
    0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE, 0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02,
    0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1, 0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
    0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B, 0xBB60ADFC, 0xB6238B25, 0xB2E29692,
    0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6, 0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A,
    0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E, 0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2,
    0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34, 0xDC3ABDED, 0xD8FBA05A,
    0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637, 0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB,
    0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F, 0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
    0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5, 0x3F9B762C, 0x3B5A6B9B,
    0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF, 0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623,
    0xF12F560E, 0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B,
    0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
    0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7, 0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B,
    0x9B3660C6, 0x9FF77D71, 0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
    0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640, 0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C,
    0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8, 0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24,
    0x119B4BE9, 0x155A565E, 0x18197087, 0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
    0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D, 0x2056CD3A, 0x2D15EBE3, 0x29D4F654,
    0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0, 0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C,
    0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18, 0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
    0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662, 0x933EB0BB, 0x97FFAD0C,
    0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668, 0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4,
};

static uint32_t ogg_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
    return crc;
}

/* ogg_crc continued over more bytes */
static uint32_t ogg_crc_cont(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
    return crc;
}

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
} growbuf;

static void growbuf_put(growbuf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + n)
            ncap *= 2;
        uint8_t *np = realloc(b->p, ncap);
        if (!np)
            return;
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

typedef struct {
    growbuf out;
    uint32_t serial;
    uint32_t seq;
    int bos;                 /* BOS flag still pending for first page */
    uint8_t lacing[255];
    int nseg;
    uint8_t body[255 * 255];
    size_t body_len;
    uint64_t granule;
    int granule_set;
    int eos;
    int continued;
} oggw;

static void oggw_init(oggw *w)
{
    memset(w, 0, sizeof(*w));
    w->serial = 1;
    w->bos = 1;
}

static void oggw_flush(oggw *w)
{
    if (w->nseg == 0)
        return;
    uint8_t header[27 + 255];
    size_t hl = (size_t)27 + w->nseg;
    memcpy(header, "OggS", 4);
    header[4] = 0;
    uint8_t htype = 0;
    if (w->bos)
        htype |= 2;
    if (w->continued)
        htype |= 1;
    if (w->eos)
        htype |= 4;
    header[5] = htype;
    uint64_t g = w->granule_set ? w->granule : 0xFFFFFFFFFFFFFFFFULL;
    memcpy(header + 6, &g, 8);
    memcpy(header + 14, &w->serial, 4);
    uint32_t seq = w->seq++;
    memcpy(header + 18, &seq, 4);
    memset(header + 22, 0, 4);     /* CRC placeholder */
    header[26] = (uint8_t)w->nseg;
    memcpy(header + 27, w->lacing, w->nseg);

    uint32_t crc = ogg_crc(header, hl);
    crc = ogg_crc_cont(crc, w->body, w->body_len);
    memcpy(header + 22, &crc, 4);

    growbuf_put(&w->out, header, hl);
    growbuf_put(&w->out, w->body, w->body_len);

    w->nseg = 0;
    w->body_len = 0;
    w->eos = 0;
    w->bos = 0;
    w->granule_set = 0;
    w->continued = 0;
}

/* Append a packet.  Does not flush; caller flushes at appropriate points. */
static void oggw_packet(oggw *w, uint64_t granule, int eos,
                        const uint8_t *data, size_t len)
{
    uint8_t seg[300];
    int nseg;
    if (len == 0) {
        seg[0] = 0;
        nseg = 1;
    } else {
        int full = (int)(len / 255);
        int rem = (int)(len % 255);
        nseg = 0;
        for (int i = 0; i < full; i++)
            seg[nseg++] = 255;
        if (rem == 0)
            seg[nseg++] = 0;     /* terminating 0 for exact multiples */
        else
            seg[nseg++] = (uint8_t)rem;
    }

    if (w->nseg > 0 && w->nseg + nseg > 255)
        oggw_flush(w);

    for (int i = 0; i < nseg; i++)
        w->lacing[w->nseg++] = seg[i];
    if (len > 0)
        memcpy(w->body + w->body_len, data, len);
    w->body_len += len;

    w->granule = granule;
    w->granule_set = 1;
    w->eos = eos;
}

/* ------------------------------------------------------------------ */
/* Vorbis header packet builders                                       */
/* ------------------------------------------------------------------ */
static size_t build_info(uint8_t *b, int channels, uint32_t freq)
{
    size_t n = 0;
    b[n++] = 0x01;
    memcpy(b + n, "vorbis", 6); n += 6;
    uint32_t v = 0; memcpy(b + n, &v, 4); n += 4;       /* version */
    b[n++] = (uint8_t)channels;
    memcpy(b + n, &freq, 4); n += 4;                    /* sample rate */
    v = 0; memcpy(b + n, &v, 4); n += 4;               /* bitrate max */
    memcpy(b + n, &v, 4); n += 4;                       /* bitrate nominal */
    memcpy(b + n, &v, 4); n += 4;                       /* bitrate min */
	/* blocksize: log2(2048)=11 in high nibble, log2(256)=8 in low nibble */
	b[n++] = (uint8_t)((11 << 4) | 8);
    b[n++] = 1;                                          /* framing */
    return n;
}

static size_t build_comment(uint8_t *b)
{
    size_t n = 0;
    b[n++] = 0x03;
    memcpy(b + n, "vorbis", 6); n += 6;
    const char *vendor = "Fmod5Sharp (Samboy063)";
    uint32_t vlen = (uint32_t)strlen(vendor);
    memcpy(b + n, &vlen, 4); n += 4;
    memcpy(b + n, vendor, vlen); n += vlen;
    uint32_t z = 0;
    memcpy(b + n, &z, 4); n += 4;       /* comment count = 0 */
    b[n++] = 1;                          /* framing bit */
    return n;
}

/* ------------------------------------------------------------------ */
/* Rebuild                                                             */
/* ------------------------------------------------------------------ */
int vorbis_rebuild_sample(const fsb5_sample *s, uint8_t **out, size_t *out_len)
{
    if (s->crc == 0)
        return -1;

    const uint8_t *setup;
    uint32_t setup_len;
    int32_t seekbit;
    if (!vorbis_db_lookup(s->crc, &setup, &setup_len, &seekbit))
        return -1;

    int blockflag[64];
    int num_modes = 0;
    if (compute_block_flags(setup, setup_len, seekbit, blockflag, &num_modes) < 0)
        return -1;
    if (num_modes <= 0 || num_modes > 64)
        return -1;

    oggw w;
    oggw_init(&w);

    uint8_t info[64];
    size_t il = build_info(info, s->channels, s->frequency);
    oggw_packet(&w, 0, 0, info, il);
    oggw_flush(&w);                 /* BOS page: info header only */

    uint8_t comment[64];
    size_t cl = build_comment(comment);
    oggw_packet(&w, 0, 0, comment, cl);
    oggw_flush(&w);                 /* comment header page */

    oggw_packet(&w, 0, 0, setup, setup_len);
    oggw_flush(&w);                 /* setup header page */

    const uint8_t *raw = s->data;
    size_t rawlen = s->data_length;
    size_t idx = 0;
    uint64_t granule = 0;
    int prev_bs = 0;
    int done = 0;

    while (idx + 2 <= rawlen && !done) {
        uint16_t ln;
        memcpy(&ln, raw + idx, 2);            /* uint16 LE packet length */
        if (ln == 0 || ln == 0xFFFF)
            break;
        idx += 2;
        if (idx + ln > rawlen)
            break;
        const uint8_t *pkt = raw + idx;
        idx += ln;

        int bs = packet_block_size(pkt, ln, blockflag, num_modes);
        if (prev_bs == 0)
            granule = 0;
        else
            granule += (uint64_t)(bs + prev_bs) / 4;
        prev_bs = bs;

        /* EOS when the next packet is a terminator or the buffer ends. */
        int eos = 0;
        if (idx + 2 > rawlen) {
            eos = 1;
        } else {
            uint16_t nxt;
            memcpy(&nxt, raw + idx, 2);
            if (nxt == 0 || nxt == 0xFFFF)
                eos = 1;
        }

        oggw_packet(&w, granule, eos, pkt, ln);
        oggw_flush(&w);
        if (eos)
            done = 1;
    }

    oggw_flush(&w);                 /* final flush (no-op if empty) */

    if (w.out.len == 0) {
        free(w.out.p);
        return -1;
    }
    *out = w.out.p;
    *out_len = w.out.len;
    return 0;
}
