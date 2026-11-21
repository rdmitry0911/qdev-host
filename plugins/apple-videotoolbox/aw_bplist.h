/*
 * aw_bplist.h - a minimal Apple binary property list (bplist00) ENCODER.
 *
 * The shipping x86_64 VideoToolbox framework parses the host's Dpba attributes,
 * the decoder/encoder lists (,dec / ,dca / ,enc) and ,hin as binary property
 * lists via CFPropertyListCreateWithData (contract-B §3, "Property lists are
 * binary bplist00"). This encoder produces those bytes; it is the gate from the
 * host-only proof (=pls NULL placeholders) to a real guest.
 *
 * Scope: the object types a decode-path dict needs - dict, array, ASCII string,
 * signed integer, boolean, data. No reals/dates/UID/unicode (not needed here).
 * Output is a well-formed bplist00 (header, objects, offset table, 32-byte
 * trailer) that CoreFoundation's binary-plist reader accepts; aw_bplist_check()
 * re-parses it as the host-side proof of well-formedness.
 *
 * CLEAN-ROOM from the public bplist00 format. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#ifndef AW_BPLIST_H
#define AW_BPLIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AW_BPL_MAX_OBJECTS 256
#define AW_BPL_MAX_CHILDREN 32

typedef struct aw_bplist aw_bplist;

/* Create a builder, or NULL on OOM. */
aw_bplist *aw_bplist_new(void);
void       aw_bplist_free(aw_bplist *b);

/* Add a primitive; returns its object reference (>= 0), or <0 on overflow. */
int aw_bpl_bool(aw_bplist *b, int v);
int aw_bpl_int(aw_bplist *b, int64_t v);
int aw_bpl_string(aw_bplist *b, const char *s);      /* ASCII */
int aw_bpl_data(aw_bplist *b, const uint8_t *p, size_t n);

/* Add a container referencing earlier objects by ref; returns its ref or <0. */
int aw_bpl_array(aw_bplist *b, const int *refs, int n);
int aw_bpl_dict(aw_bplist *b, const int *key_refs, const int *val_refs, int n);

/* Serialize with `root` as the top object into out[cap]. Returns the byte
 * length, or 0 on overflow / a bad ref. */
size_t aw_bplist_finalize(aw_bplist *b, int root, uint8_t *out, size_t cap);

/* Host-side proof: validate that `buf` is a structurally well-formed bplist00
 * (magic, trailer consistent, every offset/ref in range, top object a container
 * — a dict or an array).
 * Returns 0 if well-formed, <0 otherwise. This stands in for a round-trip
 * through CFPropertyListCreateWithData, which is unavailable off a Mac. */
int aw_bplist_check(const uint8_t *buf, size_t len);

/* Read a bplist00 whose TOP object is a string (ASCII 0x5N, or UTF-16BE 0x6N
 * with only ASCII code points) into out[cap] (NUL-terminated). Returns the
 * string length, or <0 if the buffer is not such a plist or out is too small.
 * Used for the guest's dcop ,key (VTParavirtualizationMessageAppendCFPropertyList
 * of a CFString). */
int aw_bplist_read_string(const uint8_t *buf, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AW_BPLIST_H */
