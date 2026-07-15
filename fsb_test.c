#define _GNU_SOURCE
#include <string.h>
/*
 * Standalone test: parse a .bank's FSB5 blob, rebuild one named sample to a
 * valid OGG, and (optionally) report its size so it can be diffed against the
 * python-fsb5 oracle.  Build with:
 *   cc -o /tmp/fsb_test fsb_test.c fsb.c vorbis_ogg.c vorbis_db.c -lvorbisfile
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsb.h"
#include "vorbis_ogg.h"

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { fclose(fp); free(buf); return NULL; }
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bank.bank> <sample_name> [out.ogg]\n", argv[0]);
        return 2;
    }
    size_t bank_size;
    uint8_t *bank = read_file(argv[1], &bank_size);
    if (!bank) return 1;
    uint8_t *fsb = memmem(bank, bank_size, (const uint8_t *)"FSB5", 4);
    if (!fsb) { fprintf(stderr, "FSB5 not found\n"); return 1; }
    size_t fsb_size = bank_size - (fsb - bank);

    fsb5_header hdr;
    fsb5_sample *samples = NULL;
    if (fsb5_parse(fsb, fsb_size, &hdr, &samples) < 0) {
        fprintf(stderr, "fsb5_parse failed\n");
        return 1;
    }
    fprintf(stderr, "samples=%u audio_type=%u\n", hdr.num_samples, hdr.audio_type);

    fsb5_sample *target = NULL;
    for (uint32_t i = 0; i < hdr.num_samples; i++) {
        if (samples[i].name && strcmp((char *)samples[i].name, argv[2]) == 0) {
            target = &samples[i];
            fprintf(stderr, "found idx=%u ch=%d freq=%u crc=%08x raw=%u\n",
                    i, samples[i].channels, samples[i].frequency,
                    samples[i].crc, samples[i].data_length);
            break;
        }
    }
    if (!target) { fprintf(stderr, "sample not found\n"); return 1; }

    uint8_t *out = NULL;
    size_t out_len = 0;
    if (vorbis_rebuild_sample(target, &out, &out_len) < 0) {
        fprintf(stderr, "rebuild failed\n");
        return 1;
    }
    fprintf(stderr, "rebuilt %zu bytes\n", out_len);
    if (argc >= 4) {
        FILE *o = fopen(argv[3], "wb");
        if (o) { fwrite(out, 1, out_len, o); fclose(o); }
    } else {
        fwrite(out, 1, out_len, stdout);
    }
    free(out);
    fsb5_free(&hdr, samples);
    free(bank);
    return 0;
}
