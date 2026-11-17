/*
 * aw_sbuf.h - minimal CMSampleBuffer (`sbuf`) serialization for the Apple-wire
 * decode subset (contract-B §6, WIRE-CONTRACT §8). The `,sbf` parameter of a
 * `dfrm` carries one of these.
 *
 * An `sbuf` atom's children are, in writer order: opts, stia, sdat, nsmp, ssiz,
 * fdsc, crpt, satt/sat2/sary. For a DECODE MVP the host needs only `sdat` (the
 * access-unit bytes) and `stia` (CMSampleTimingInfo). The 212-key attachment
 * table is encode/round-trip scope and is NOT handled here.
 *
 * ENDIANNESS (contract-B §6): atom headers (`[u32 size][fourcc]`) are BIG-endian;
 * the scalars INSIDE a child (stia's CMTime fields, ssiz's i32 sizes) are NATIVE
 * (little-endian on x86_64). This module is the one place that rule applies.
 *
 * CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#ifndef AW_SBUF_H
#define AW_SBUF_H

#include <stddef.h>
#include <stdint.h>

#include "aw_wire.h" /* AW_4CC, BE helpers */

#ifdef __cplusplus
extern "C" {
#endif

#define AW_ATOM_SBUF_OPTS AW_4CC('o','p','t','s')
#define AW_ATOM_SBUF_STIA AW_4CC('s','t','i','a')
#define AW_ATOM_SBUF_SDAT AW_4CC('s','d','a','t')
#define AW_ATOM_SBUF_NSMP AW_4CC('n','s','m','p')
#define AW_ATOM_SBUF_SSIZ AW_4CC('s','s','i','z')
#define AW_ATOM_SBUF_FDSC AW_4CC('f','d','s','c')

/* CMTime, 24 bytes, NATIVE scalars (contract-B §6). */
typedef struct aw_cmtime {
    int64_t  value;
    int32_t  timescale;
    uint32_t flags;
    int64_t  epoch;
} aw_cmtime;

/* CMSampleTimingInfo, 72 bytes = three CMTimes, NATIVE. */
typedef struct aw_sample_timing {
    aw_cmtime duration;
    aw_cmtime pts;   /* presentationTimeStamp */
    aw_cmtime dts;   /* decodeTimeStamp       */
} aw_sample_timing;

/* Write an `sbuf` atom carrying one `stia` (when `timing` != NULL) and one
 * `sdat` (the access unit). Returns the total `sbuf` length, or 0 on overflow.
 * The caller wraps the result in a `,sbf` para (aw_put_bytes). */
size_t aw_sbuf_write(uint8_t *buf, size_t cap,
                     const aw_sample_timing *timing,
                     const uint8_t *au, size_t au_len);

/* Parse an `sbuf` atom. On success returns 0 and sets the au/au_len outputs to the `sdat`
 * bytes (pointer into `sbuf`, not copied). If `timing_out` != NULL and a `stia`
 * is present, its first CMSampleTimingInfo is decoded (native scalars) and
 * *has_timing set to 1. Returns <0 (an AW_ERR_* / AW_PARSE_BAD) when `sbuf` is
 * not an `sbuf` atom, is truncated, has a second stia/ssiz/fdsc, or has no
 * `sdat`. Unknown child atoms are skipped. */
int aw_sbuf_read(const uint8_t *sbuf, size_t len,
                 const uint8_t **au, size_t *au_len,
                 aw_sample_timing *timing_out, int *has_timing);

#ifdef __cplusplus
}
#endif

#endif /* AW_SBUF_H */
