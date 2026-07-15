#ifndef VORBIS_OGG_H
#define VORBIS_OGG_H

#include <stdint.h>
#include <stddef.h>
#include "fsb.h"

/* Lookup a Vorbis setup packet in the embedded setup database by crc32.
 * Returns 1 on success (fills *hdr/hdrlen/seekbit), 0 if not found. */
int vorbis_db_lookup(uint32_t crc, const uint8_t **hdr,
                     uint32_t *hdrlen, int32_t *seekbit);

/* Rebuild a valid OGG Vorbis stream from an FSB5 Vorbis sample using the
 * crc32 -> setup database.  On success allocates *out (caller frees with
 * free()) and returns 0; returns -1 on error (e.g. crc not present in DB,
 * or sample is not Vorbis). */
int vorbis_rebuild_sample(const fsb5_sample *sample,
                          uint8_t **out, size_t *out_len);

#endif
