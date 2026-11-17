/*
 * aw_sbuf.c - minimal CMSampleBuffer serialization. See aw_sbuf.h and the
 * WIRE-CONTRACT §8. CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_sbuf.h"

#include <string.h>

/* NATIVE (little-endian) scalar access for the bytes inside a child atom. */
static void le_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void le_put64(uint8_t *p, uint64_t v) {
    le_put32(p, (uint32_t)v); le_put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t le_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le_get64(const uint8_t *p) {
    return (uint64_t)le_get32(p) | ((uint64_t)le_get32(p + 4) << 32);
}

static void put_cmtime(uint8_t *p, const aw_cmtime *t) {
    le_put64(p + 0,  (uint64_t)t->value);
    le_put32(p + 8,  (uint32_t)t->timescale);
    le_put32(p + 12, t->flags);
    le_put64(p + 16, (uint64_t)t->epoch);
}
static void get_cmtime(const uint8_t *p, aw_cmtime *t) {
    t->value     = (int64_t)le_get64(p + 0);
    t->timescale = (int32_t)le_get32(p + 8);
    t->flags     = le_get32(p + 12);
    t->epoch     = (int64_t)le_get64(p + 16);
}

size_t aw_sbuf_write(uint8_t *buf, size_t cap,
                     const aw_sample_timing *timing,
                     const uint8_t *au, size_t au_len)
{
    size_t stia_box = timing ? (8 + 72) : 0;
    size_t sdat_box = 8 + au_len;
    size_t total    = 8 + stia_box + sdat_box; /* sbuf header + children */
    if (cap < total) return 0;

    uint8_t *p = buf;
    aw_put_be32(p + 0, (uint32_t)total);
    aw_put_be32(p + 4, AW_ATOM_SBUF);
    p += 8;

    if (timing) {
        aw_put_be32(p + 0, (uint32_t)stia_box); /* BE atom header */
        aw_put_be32(p + 4, AW_ATOM_SBUF_STIA);
        put_cmtime(p + 8 + 0,  &timing->duration); /* NATIVE scalars inside */
        put_cmtime(p + 8 + 24, &timing->pts);
        put_cmtime(p + 8 + 48, &timing->dts);
        p += stia_box;
    }

    aw_put_be32(p + 0, (uint32_t)sdat_box);
    aw_put_be32(p + 4, AW_ATOM_SBUF_SDAT);
    if (au_len) memcpy(p + 8, au, au_len);
    return total;
}

int aw_sbuf_read(const uint8_t *sbuf, size_t len,
                 const uint8_t **au, size_t *au_len,
                 aw_sample_timing *timing_out, int *has_timing)
{
    if (has_timing) *has_timing = 0;
    if (!sbuf || len < 8) return AW_PARSE_BAD;
    if (aw_get_be32(sbuf + 0) != (uint32_t)len && aw_get_be32(sbuf + 0) > len)
        return AW_PARSE_BAD;
    uint32_t root = aw_get_be32(sbuf + 4);
    if (root != AW_ATOM_SBUF) return AW_PARSE_BAD;
    uint32_t rootsz = aw_get_be32(sbuf + 0);
    if (rootsz < 8 || rootsz > len) return AW_PARSE_BAD;

    const uint8_t *found_sdat = NULL;
    size_t found_sdat_len = 0;
    int seen_stia = 0, seen_sdat = 0;

    size_t off = 8;
    while (off + 8 <= rootsz) {
        uint32_t asz = aw_get_be32(sbuf + off);      /* BE atom header */
        uint32_t fcc = aw_get_be32(sbuf + off + 4);
        if (asz < 8 || asz > rootsz - off) return AW_PARSE_BAD;
        const uint8_t *body = sbuf + off + 8;
        size_t body_len = asz - 8;

        if (fcc == AW_ATOM_SBUF_SDAT) {
            if (seen_sdat) return AW_PARSE_BAD; /* reader refuses duplicates */
            found_sdat = body;
            found_sdat_len = body_len;
            seen_sdat = 1;
        } else if (fcc == AW_ATOM_SBUF_STIA) {
            if (seen_stia) return AW_PARSE_BAD;  /* a second stia is malformed */
            seen_stia = 1;
            if (body_len < 72) return AW_PARSE_BAD;
            if (timing_out) {
                get_cmtime(body + 0,  &timing_out->duration); /* NATIVE */
                get_cmtime(body + 24, &timing_out->pts);
                get_cmtime(body + 48, &timing_out->dts);
            }
            if (has_timing) *has_timing = 1;
        }
        /* unknown atoms (opts/nsmp/ssiz/fdsc/attachments) are skipped here */
        off += asz;
    }

    if (!seen_sdat) return AW_PARSE_BAD;
    *au = found_sdat;
    *au_len = found_sdat_len;
    return 0;
}
