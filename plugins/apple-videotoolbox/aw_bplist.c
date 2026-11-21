/*
 * aw_bplist.c - minimal bplist00 encoder. See aw_bplist.h. CLEAN-ROOM from the
 * public Apple binary-property-list format. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_bplist.h"

#include <stdlib.h>
#include <string.h>

enum { OBJ_BOOL, OBJ_INT, OBJ_STR, OBJ_DATA, OBJ_ARRAY, OBJ_DICT };

typedef struct {
    int      type;
    int64_t  ival;                 /* bool/int                               */
    char    *str;                  /* string (owned)                         */
    uint8_t *data; size_t data_len;/* data (owned)                           */
    int      kids[AW_BPL_MAX_CHILDREN];  /* array elems, or dict value refs  */
    int      keys[AW_BPL_MAX_CHILDREN];  /* dict key refs                    */
    int      nkids;
} aw_obj;

struct aw_bplist {
    aw_obj obj[AW_BPL_MAX_OBJECTS];
    int    n;
};

aw_bplist *aw_bplist_new(void) { return calloc(1, sizeof(aw_bplist)); }

void aw_bplist_free(aw_bplist *b)
{
    if (!b) return;
    for (int i = 0; i < b->n; i++) { free(b->obj[i].str); free(b->obj[i].data); }
    free(b);
}

static int add(aw_bplist *b, int type)
{
    if (b->n >= AW_BPL_MAX_OBJECTS) return -1;
    aw_obj *o = &b->obj[b->n];
    memset(o, 0, sizeof(*o));
    o->type = type;
    return b->n++;
}

int aw_bpl_bool(aw_bplist *b, int v)
{
    int r = add(b, OBJ_BOOL); if (r < 0) return -1;
    b->obj[r].ival = v ? 1 : 0; return r;
}
int aw_bpl_int(aw_bplist *b, int64_t v)
{
    int r = add(b, OBJ_INT); if (r < 0) return -1;
    b->obj[r].ival = v; return r;
}
int aw_bpl_string(aw_bplist *b, const char *s)
{
    int r = add(b, OBJ_STR); if (r < 0) return -1;
    if (!s) s = "";
    size_t n = strlen(s);
    b->obj[r].str = malloc(n + 1);
    if (!b->obj[r].str) { b->n--; return -1; }
    memcpy(b->obj[r].str, s, n + 1);
    return r;
}
int aw_bpl_data(aw_bplist *b, const uint8_t *p, size_t n)
{
    int r = add(b, OBJ_DATA); if (r < 0) return -1;
    b->obj[r].data = malloc(n ? n : 1);
    if (!b->obj[r].data) { b->n--; return -1; }
    if (n) memcpy(b->obj[r].data, p, n);
    b->obj[r].data_len = n; return r;
}
int aw_bpl_array(aw_bplist *b, const int *refs, int n)
{
    if (n > AW_BPL_MAX_CHILDREN) return -1;
    int r = add(b, OBJ_ARRAY); if (r < 0) return -1;
    b->obj[r].nkids = n;
    for (int i = 0; i < n; i++) b->obj[r].kids[i] = refs[i];
    return r;
}
int aw_bpl_dict(aw_bplist *b, const int *key_refs, const int *val_refs, int n)
{
    if (n > AW_BPL_MAX_CHILDREN) return -1;
    int r = add(b, OBJ_DICT); if (r < 0) return -1;
    b->obj[r].nkids = n;
    for (int i = 0; i < n; i++) { b->obj[r].keys[i] = key_refs[i]; b->obj[r].kids[i] = val_refs[i]; }
    return r;
}

/* ---- serialization ------------------------------------------------------- */

static void put_be(uint8_t *p, uint64_t v, int nbytes)
{
    for (int i = 0; i < nbytes; i++) p[i] = (uint8_t)(v >> (8 * (nbytes - 1 - i)));
}

/* number of bytes (1/2/4/8) to hold a bplist integer value */
static int int_width(int64_t v)
{
    if (v < 0) return 8;
    if (v <= 0xff) return 1;
    if (v <= 0xffff) return 2;
    if (v <= 0xffffffffLL) return 4;
    return 8;
}
static int int_marker_log2(int w) { return w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3; }

/* append a type|count marker; counts >= 15 spill into a following int object */
static size_t emit_marker(uint8_t *out, size_t off, uint8_t type_nibble, size_t count)
{
    if (count < 15) {
        out[off++] = (uint8_t)((type_nibble << 4) | count);
    } else {
        out[off++] = (uint8_t)((type_nibble << 4) | 0xf);
        int w = int_width((int64_t)count);
        out[off++] = (uint8_t)(0x10 | int_marker_log2(w));
        put_be(out + off, (uint64_t)count, w); off += w;
    }
    return off;
}

size_t aw_bplist_finalize(aw_bplist *b, int root, uint8_t *out, size_t cap)
{
    if (!b || root < 0 || root >= b->n || b->n == 0) return 0;
    const int nobj = b->n;
    int ref_size = nobj <= 0xff ? 1 : 2;             /* bytes per object ref  */
    size_t offsets[AW_BPL_MAX_OBJECTS];

    /* 1) serialize the header + every object in index order (offsets recorded). */
    if (cap < 8) return 0;
    memcpy(out, "bplist00", 8);
    size_t off = 8;

    for (int i = 0; i < nobj; i++) {
        offsets[i] = off;
        const aw_obj *o = &b->obj[i];
        /* bytes this object needs: marker (1) + a possible count-spill int
         * (1+8) + its payload. */
        size_t need = 11;
        if (o->type == OBJ_STR)        need += strlen(o->str);
        else if (o->type == OBJ_DATA)  need += o->data_len;
        else if (o->type == OBJ_ARRAY) need += (size_t)o->nkids * ref_size;
        else if (o->type == OBJ_DICT)  need += (size_t)o->nkids * 2 * ref_size;
        if (off + need > cap) return 0;
        switch (o->type) {
        case OBJ_BOOL: out[off++] = o->ival ? 0x09 : 0x08; break;
        case OBJ_INT: {
            int w = int_width(o->ival);
            out[off++] = (uint8_t)(0x10 | int_marker_log2(w));
            put_be(out + off, (uint64_t)o->ival, w); off += w;
            break;
        }
        case OBJ_STR: {
            size_t n = strlen(o->str);
            off = emit_marker(out, off, 0x5, n);     /* ASCII string           */
            memcpy(out + off, o->str, n); off += n;
            break;
        }
        case OBJ_DATA:
            off = emit_marker(out, off, 0x4, o->data_len);
            memcpy(out + off, o->data, o->data_len); off += o->data_len;
            break;
        case OBJ_ARRAY:
            off = emit_marker(out, off, 0xa, (size_t)o->nkids);
            for (int k = 0; k < o->nkids; k++) {
                put_be(out + off, (uint64_t)o->kids[k], ref_size); off += ref_size;
            }
            break;
        case OBJ_DICT:
            off = emit_marker(out, off, 0xd, (size_t)o->nkids);
            for (int k = 0; k < o->nkids; k++) {     /* all key refs ...       */
                put_be(out + off, (uint64_t)o->keys[k], ref_size); off += ref_size;
            }
            for (int k = 0; k < o->nkids; k++) {     /* ... then all val refs  */
                put_be(out + off, (uint64_t)o->kids[k], ref_size); off += ref_size;
            }
            break;
        default: return 0;
        }
    }

    /* 2) offset table. */
    size_t table_start = off;
    int off_size = table_start <= 0xff ? 1 : table_start <= 0xffff ? 2 : 4;
    if (off + (size_t)nobj * off_size + 32 > cap) return 0;
    for (int i = 0; i < nobj; i++) { put_be(out + off, (uint64_t)offsets[i], off_size); off += off_size; }

    /* 3) 32-byte trailer. */
    memset(out + off, 0, 6); off += 6;               /* unused                 */
    out[off++] = (uint8_t)off_size;                  /* offset int size        */
    out[off++] = (uint8_t)ref_size;                  /* object ref size        */
    put_be(out + off, (uint64_t)nobj, 8); off += 8;  /* num objects            */
    put_be(out + off, (uint64_t)root, 8); off += 8;  /* top object index       */
    put_be(out + off, (uint64_t)table_start, 8); off += 8; /* offset table start*/
    return off;
}

/* ---- validator (stands in for CFPropertyListCreateWithData) -------------- */

int aw_bplist_check(const uint8_t *buf, size_t len)
{
    if (!buf || len < 8 + 32) return -1;
    if (memcmp(buf, "bplist00", 8) != 0) return -1;
    const uint8_t *tr = buf + len - 32;
    int off_size = tr[6], ref_size = tr[7];
    if (off_size < 1 || off_size > 8 || ref_size < 1 || ref_size > 8) return -1;
    uint64_t nobj = 0, top = 0, table = 0;
    for (int i = 0; i < 8; i++) {
        nobj  = (nobj  << 8) | tr[8 + i];
        top   = (top   << 8) | tr[16 + i];
        table = (table << 8) | tr[24 + i];
    }
    if (nobj == 0 || top >= nobj) return -1;
    if (table + nobj * off_size + 32 != len) return -1; /* table + trailer fit  */
    /* every offset must land inside the object region */
    for (uint64_t i = 0; i < nobj; i++) {
        uint64_t o = 0;
        for (int k = 0; k < off_size; k++) o = (o << 8) | buf[table + i * off_size + k];
        if (o < 8 || o >= table) return -1;
    }
    /* the top object must be a container: a dict (0xd) or an array (0xa) */
    uint64_t toff = 0;
    for (int k = 0; k < off_size; k++) toff = (toff << 8) | buf[table + top * off_size + k];
    uint8_t top_nibble = buf[toff] >> 4;
    if (top_nibble != 0xd && top_nibble != 0xa) return -1;
    return 0;
}

int aw_bplist_read_string(const uint8_t *buf, size_t len, char *out, size_t cap)
{
    if (!buf || !out || !cap || len < 8 + 32 || memcmp(buf, "bplist00", 8) != 0) return -1;
    const uint8_t *tr = buf + len - 32;
    int off_size = tr[6];
    if (off_size < 1 || off_size > 8) return -1;
    uint64_t nobj = 0, top = 0, table = 0;
    for (int i = 0; i < 8; i++) {
        nobj  = (nobj  << 8) | tr[8 + i];
        top   = (top   << 8) | tr[16 + i];
        table = (table << 8) | tr[24 + i];
    }
    if (nobj == 0 || top >= nobj || table >= len || table + nobj * (uint64_t)off_size + 32 != len) return -1;
    uint64_t o = 0;
    for (int k = 0; k < off_size; k++) o = (o << 8) | buf[table + top * off_size + k];
    if (o < 8 || o >= table) return -1;
    uint8_t m = buf[o], kind = m >> 4;
    uint64_t n = m & 0xf, p = o + 1;
    if (kind != 0x5 && kind != 0x6) return -1;
    if (n == 0xf) {                         /* length follows as an int object */
        if (p >= table || (buf[p] >> 4) != 0x1) return -1;
        int ib = 1 << (buf[p] & 0xf);
        if (ib > 8 || p + 1 + (uint64_t)ib > table) return -1;
        n = 0;
        for (int k = 0; k < ib; k++) n = (n << 8) | buf[p + 1 + k];
        p += 1 + (uint64_t)ib;
    }
    uint64_t bytes = kind == 0x5 ? n : 2 * n;
    if (p + bytes > table || n + 1 > cap) return -1;
    for (uint64_t i = 0; i < n; i++) {
        if (kind == 0x5) out[i] = (char)buf[p + i];
        else {
            if (buf[p + 2 * i] != 0 || buf[p + 2 * i + 1] >= 0x80) return -1;
            out[i] = (char)buf[p + 2 * i + 1];
        }
    }
    out[n] = 0;
    return (int)n;
}
