#ifndef FSB_H
#define FSB_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t version;
    uint32_t num_samples;
    uint32_t sample_headers_size;
    uint32_t name_table_size;
    uint32_t data_size;
    uint32_t audio_type;       /* 15 = VORBIS; see fsb5_parse */
    uint8_t hash[16];
} fsb5_header;

typedef struct {
    uint32_t data_offset;
    uint32_t data_length;
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t frequency;
    uint32_t sample_count;     /* total PCM samples (drives OGG granule / EOS) */
    uint32_t crc;              /* VORBISDATA crc32 -> vorbis setup DB key */
    float volume;              /* PEAKVOLUME chunk (type 13); clamped to >= 0.25 */
    int16_t pan;
    int16_t priority;
    int16_t channels;
    int16_t block_align;
    int16_t mode_bits;
    int16_t reserved;
    uint32_t name_hash;
    uint32_t name_offset;
    uint8_t *name;
    uint8_t *data;             /* raw FSB5 sample bytes; after extract, holds final OGG bytes */
    uint8_t *ogg_data;         /* rebuilt OGG bytes (vorbis_rebuild_sample output) */
    size_t ogg_len;            /* length of ogg_data */
} fsb5_sample;

int fsb5_parse(const uint8_t *data, size_t size, fsb5_header *header,
               fsb5_sample **samples);
void fsb5_free(fsb5_header *header, fsb5_sample *samples);
int fsb5_extract_samples(const uint8_t *data, size_t size, fsb5_header *header,
                          fsb5_sample *samples);

#endif
