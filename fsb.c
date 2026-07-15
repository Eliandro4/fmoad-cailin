#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "fmoad-cailin.h"
#include "fsb.h"

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t copy_len = (src_len >= size) ? size - 1 : src_len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);
    if (dst_len >= size) return size + src_len;
    
    size_t copy_len = size - dst_len - 1;
    if (src_len < copy_len) copy_len = src_len;
    
    memcpy(dst + dst_len, src, copy_len);
    dst[dst_len + copy_len] = '\0';
    return dst_len + src_len;
}


/* Little-endian "FSB5" */
#define FSB5_MAGIC 0x35425346

/* Fmod5Sharp Frequencies table, indexed by the sample header frequency id
 * (IMPLEMENTATION.md §12.3.1). */
static const uint32_t freq_table[] = {
    4000, 8000, 11000, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000
};
static const int channel_map[] = {1, 2, 6, 8};

static uint32_t read_le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* Parse one FSB5 sample header.  Returns the offset just past this sample's
 * header chain, or -1 on error.
 *
 * The 64-bit "mode" word is decoded LSB-first, verified bit-for-bit against
 * the decompiled FMOD CodecFSB5::decodeSubSoundHeader (fmod_reduced.ll):
 *   bit  0      : has_any_chunks
 *   bits 1-4    : frequency id (index into freq_table)
 *   bits 5-6    : channel bits -> channel_map[]
 *   bits 7-33   : data offset (* 32) from data_start
 *   bits 34-63  : sample count (PCM samples)
 *
 * The metadata chunk chain is walked per CodecFSB5::getWaveFormatInternal /
 * decodeMetaDataHeader.  Chunk 4-byte header (LE):
 *   bit  0      : more_chunks (1 => another chunk follows)
 *   bits 1-24   : chunk_size (payload bytes)
 *   bits 25-31  : chunk_type
 * Authoritative chunk-type -> field map (from getWaveFormatInternal):
 *   1  CHANNELS   : i8  signed   -> channels override
 *   2  FREQUENCY  : i32          -> frequency override
 *   3  LOOP       : i32,i32      -> loop_start, loop_end
 *   6  XMASEEK    : seek table (xma only; ignored for vorbis)
 *   8  (seek)     : u8 -> lookup[3,4,2] (ignored)
 *   9  ATRAC9CFG  : (atrac only; ignored)
 *   11 VORBISDATA : i32 crc32 + seek bytes  -> crc key into setup DB
 *   13 PEAKVOLUME : float, clamped to >= 0.25 -> volume
 *   0/14/10/...   : length / intra-layers / xwma (ignored for ogg rebuild)
 */
static int parse_sample_header(const uint8_t *data, size_t size,
                                size_t offset, fsb5_sample *sample)
{
    if (offset + 8 > size)
        return -1;

    uint64_t mode = read_le64(data + offset);
    int next_chunk = mode & 1;
    int freq_index = (mode >> 1) & 0xF;
    int ch_bits = (mode >> 5) & 0x3;
    int channels = channel_map[ch_bits];
    uint32_t data_offset = ((mode >> 7) & 0x7FFFFFF) * 32;
    uint32_t sample_count = (uint32_t)((mode >> 34) & 0x3FFFFFFF);

    uint32_t frequency = 0;
    if (freq_index < sizeof(freq_table) / sizeof(freq_table[0]))
        frequency = freq_table[freq_index];

    sample->data_offset = data_offset;
    sample->sample_count = sample_count;
    sample->frequency = frequency;
    sample->channels = channels;
    sample->crc = 0;
    sample->volume = 1.0f;
    sample->pan = 0;
    sample->priority = 0;
    sample->block_align = 0;
    sample->mode_bits = 0;
    sample->name_hash = 0;
    sample->name_offset = 0xFFFFFFFF;
    sample->name = NULL;
    sample->data = NULL;

    size_t pos = offset + 8;
    while (next_chunk) {
        if (pos + 4 > size)
            return -1;
        uint32_t chunk_header = read_le32(data + pos);
        next_chunk = chunk_header & 1;
        uint32_t chunk_size = (chunk_header >> 1) & 0xFFFFFF;
        int chunk_type = (chunk_header >> 25) & 0x7F;
        pos += 4;

        if (pos + chunk_size > size)
            return -1;

        switch (chunk_type) {
        case 1: // CHANNELS: signed i8 override
            if (chunk_size >= 1) {
                int8_t c = (int8_t)data[pos];
                if (c > 0)
                    sample->channels = c;
            }
            break;
        case 2: // FREQUENCY (absolute Hz override)
            if (chunk_size >= 4)
                sample->frequency = read_le32(data + pos);
            break;
        case 3: // LOOP: loop_start, loop_end (i32 each)
            if (chunk_size >= 8) {
                sample->loop_start = read_le32(data + pos);
                sample->loop_end = read_le32(data + pos + 4);
            }
            break;
        case 11: // VORBISDATA: uint32 LE crc32, then seek/padding bytes
            if (chunk_size >= 4)
                sample->crc = read_le32(data + pos);
            break;
        case 13: // PEAKVOLUME: float, clamped to >= 0.25 (per getWaveFormatInternal)
            if (chunk_size >= 4) {
                uint32_t raw = read_le32(data + pos);
                float v;
                memcpy(&v, &raw, sizeof(v));
                if (v > 0.25f)
                    sample->volume = v;
                else
                    sample->volume = 0.25f;
            }
            break;
        default:
            /* CHUNK 6 (XMASEEK), 8, 9 (ATRAC9CFG), 10 (XWMADATA), 14
             * (VORBISINTRALAYERS), 0 (length), 4 (COMMENT), 7 (DSPCOEFF),
             * 15 (OPUSDATALEN): not needed for the OGG rebuild. */
            break;
        }
        pos += chunk_size;
    }

    sample->data_length = 0;
    return (int)pos;
}

int fsb5_parse(const uint8_t *data, size_t size, fsb5_header *header,
                fsb5_sample **samples)
{
    if (size < 60)
        return -1;

    if (read_le32(data) != FSB5_MAGIC)
        return -1;

    memset(header, 0, sizeof(*header));
    header->version = read_le32(data + 4);
    header->num_samples = read_le32(data + 8);
    header->sample_headers_size = read_le32(data + 12);
    header->name_table_size = read_le32(data + 16);
    header->data_size = read_le32(data + 20);
    header->audio_type = read_le32(data + 24);
    memcpy(header->hash, data + 36, 16);

    /* Validated against the decompiled CodecFSB5::readHeader
     * (fmod_reduced.ll).  FSB5 requires version == 1 and rejects the
     * encryption bit (flags & 2) at offset 0x20.  We keep the decode
     * permissive otherwise so non-Vorbis / older banks still load. */
    if (header->version != 1)
        return -1;
    {
        uint32_t flags = read_le32(data + 32);
        if (flags & 2)
            return -1;
    }

    uint32_t header_size = (header->version == 0) ? 64 : 60;
    if (size < header_size)
        return -1;

    /* The FSB5 blob may be padded slightly past its declared section total
     * (observed +4 bytes in Celeste banks); python-fsb5 tolerates this, so we
     * clamp per-sample access to the actual buffer and only fail on real
     * overruns.  Still reject a header that claims more than the buffer can
     * plausibly hold (allowing the small trailing pad). */
    {
        uint64_t total = (uint64_t)header_size
                       + header->sample_headers_size
                       + header->name_table_size
                       + header->data_size;
        if (total > size + 16)
            return -1;
    }

    *samples = calloc(header->num_samples, sizeof(fsb5_sample));
    if (!*samples)
        return -1;

    size_t sample_data_start = header_size + header->sample_headers_size + header->name_table_size;
    size_t pos = header_size;

    for (uint32_t i = 0; i < header->num_samples; i++) {
        int ret = parse_sample_header(data, size, pos, &(*samples)[i]);
        if (ret < 0)
            return -1;
        pos = (size_t)ret;
    }

    /* The chunk-chain walk and the name table both resolve offsets from
     * header_size + sample_headers_size, so an exact match of the walked
     * `pos` is not required (Celeste banks carry padding here).  The data loop
     * below validates every sample's data bounds independently. */

    for (uint32_t i = 0; i < header->num_samples; i++) {
        size_t start = sample_data_start + (*samples)[i].data_offset;
        size_t end;
        if (i + 1 < header->num_samples)
            end = sample_data_start + (*samples)[i + 1].data_offset;
        else
            end = sample_data_start + header->data_size;

        /* Clamp the final sample to the actual buffer (trailing padding). */
        if (end > size)
            end = size;

        if (start >= end || start > size)
            return -1;

        (*samples)[i].data_length = (uint32_t)(end - start);
        (*samples)[i].data = malloc((*samples)[i].data_length);
        if (!(*samples)[i].data)
            return -1;
        memcpy((*samples)[i].data, data + start, (*samples)[i].data_length);
    }

    if (header->name_table_size > 0) {
        size_t name_table_start = header_size + header->sample_headers_size;
        for (uint32_t i = 0; i < header->num_samples; i++) {
            uint32_t name_off;
            memcpy(&name_off, data + name_table_start + i * 4, sizeof(name_off));
            if (name_off < header->name_table_size) {
                const char *name_str = (const char *)(data + name_table_start + name_off);
                size_t name_len = strlen(name_str);
                if (name_len > header->name_table_size - name_off)
                    name_len = header->name_table_size - name_off;
                (*samples)[i].name = malloc(name_len + 1);
                if ((*samples)[i].name) {
                    memcpy((*samples)[i].name, name_str, name_len);
                    ((char *)(*samples)[i].name)[name_len] = '\0';
                }
            }
        }
    }

    return 0;
}

void fsb5_free(fsb5_header *header, fsb5_sample *samples)
{
    if (!samples)
        return;
    for (uint32_t i = 0; i < header->num_samples; i++) {
        free(samples[i].data);
        free(samples[i].ogg_data);
        free(samples[i].name);
    }
    free(samples);
}

#include "vorbis_ogg.h"

int fsb5_extract_samples(const uint8_t *data, size_t size, fsb5_header *header,
                          fsb5_sample *samples)
{
	(void)data;
	(void)size;

	for (uint32_t i = 0; i < header->num_samples; i++) {
		uint8_t *out = NULL;
		size_t out_len = 0;
		if (header->audio_type == 15 && samples[i].crc != 0) {
			if (vorbis_rebuild_sample(&samples[i], &out, &out_len) == 0 &&
			    out_len > 0) {
				free(samples[i].data);
				samples[i].data = NULL;
				samples[i].ogg_data = out;
				samples[i].ogg_len = out_len;
				continue;
			}
			free(out);
		}
		samples[i].ogg_data = samples[i].data;
		samples[i].ogg_len = samples[i].data_length;
		samples[i].data = NULL;
	}
	return 0;
}
