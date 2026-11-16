/*
 * test_aw_wire.c - unit tests for the Apple-wire codec (aw_wire.{h,c}).
 *
 * Two kinds of check, both against the frozen contract
 * WIRE-CONTRACT-APPLE-VIDEOTOOLBOX.md:
 *   (1) STRUCT LAYOUT vs Apple's wire - offsetof/sizeof pins + golden byte
 *       vectors asserting the exact bytes the encoder emits for the header,
 *       each para value type, and the subs/rels boxes.
 *   (2) ASYNC FRAMING - the flag set, host-request detection, and reassembly of
 *       a value cut across fragment messages.
 *
 * CLEAN-ROOM. No libva, no GPU, no guest: pure byte-level codec tests.
 * Lane L-VIDEO-IMPL, 2026-11-16.  Build/run: make test (target test-aw-wire).
 */
#include "aw_wire.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            printf("  FAIL: %s byte %zu: got %02x want %02x\n", what, i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

/* ---- (1a) struct layout pins ---------------------------------------- */

static void test_layout(void)
{
    printf("-- struct layout vs Apple's wire --\n");
    /* The 48-byte vtpv header (contract §2). */
    CHECK(sizeof(aw_hdr) == 48, "sizeof(aw_hdr)=%zu want 48", sizeof(aw_hdr));
    CHECK(offsetof(aw_hdr, atom_size) == 0x00, "aw_hdr.atom_size off");
    CHECK(offsetof(aw_hdr, vtpv)      == 0x04, "aw_hdr.vtpv off");
    CHECK(offsetof(aw_hdr, type)      == 0x08, "aw_hdr.type off");
    CHECK(offsetof(aw_hdr, flags)     == 0x0c, "aw_hdr.flags off");
    CHECK(offsetof(aw_hdr, uuid)      == 0x10, "aw_hdr.uuid off");
    CHECK(offsetof(aw_hdr, reply_id)  == 0x20, "aw_hdr.reply_id off");
    CHECK(offsetof(aw_hdr, reserved)  == 0x28, "aw_hdr.reserved off");

    /* The 16-byte para framing (contract §5). */
    CHECK(sizeof(aw_para) == 16, "sizeof(aw_para)=%zu want 16", sizeof(aw_para));
    CHECK(offsetof(aw_para, key)   == 0x08, "aw_para.key off");
    CHECK(offsetof(aw_para, vtype) == 0x0c, "aw_para.vtype off");

    /* The 0x30-byte subs box (contract §6.1). */
    CHECK(sizeof(aw_subs) == 0x30, "sizeof(aw_subs)=%zu want 0x30", sizeof(aw_subs));
    CHECK(offsetof(aw_subs, ids) == 0x08, "aw_subs.ids off");

    /* FourCC packs big-endian: AW_ATOM_VTPV stored BE == 'v','t','p','v'. */
    uint8_t fcc[4];
    aw_put_be32(fcc, AW_ATOM_VTPV);
    CHECK(bytes_eq(fcc, (const uint8_t *)"vtpv", 4, "AW_ATOM_VTPV"), "vtpv fourcc");
    aw_put_be32(fcc, AW_MSG_DFRM);
    CHECK(bytes_eq(fcc, (const uint8_t *)"dfrm", 4, "AW_MSG_DFRM"), "dfrm fourcc");
    aw_put_be32(fcc, AW_KEY_FDS);
    CHECK(bytes_eq(fcc, (const uint8_t *)",fds", 4, "AW_KEY_FDS"), ",fds fourcc");
    aw_put_be32(fcc, AW_VT_U32);
    CHECK(bytes_eq(fcc, (const uint8_t *)"=u32", 4, "AW_VT_U32"), "=u32 fourcc");
    aw_put_be32(fcc, AW_MSG_DPBA);
    CHECK(bytes_eq(fcc, (const uint8_t *)"Dpba", 4, "AW_MSG_DPBA"), "Dpba fourcc");
}

/* ---- (1b) golden header + para byte vectors ------------------------- */

static void test_golden_header(void)
{
    printf("-- golden header bytes --\n");
    uint8_t buf[64];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    CHECK(aw_build_header(&b, AW_MSG_DECS, AW_FLAG_GUEST_REQ, NULL, 100) == 0, "hdr emit");
    size_t len = aw_build_finalize(&b);
    CHECK(len == 48, "header-only len=%zu want 48", len);

    static const uint8_t want[48] = {
        0x00,0x00,0x00,0x30,              /* atom_size 48 (finalize)          */
        'v','t','p','v',                  /* 'vtpv'                           */
        'd','e','c','s',                  /* type 'decs'                      */
        0x00,0x00,0x00,0x01,              /* flags AW_FLAG_GUEST_REQ          */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* uuid (zero)                      */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x64, /* reply_id 100              */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* reserved 0                */
    };
    CHECK(bytes_eq(buf, want, 48, "decs header"), "header golden");
}

static void test_golden_para(void)
{
    printf("-- golden para bytes --\n");
    uint8_t buf[256];
    aw_builder b;

    /* =u32 ,ver = 6 : 16-byte framing + 4-byte BE payload = 20 bytes. */
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DECS, AW_FLAG_HOST_REPLY, NULL, 100);
    CHECK(aw_put_u32(&b, AW_KEY_VER, 6) == 0, "put ,ver");
    size_t len = aw_build_finalize(&b);
    CHECK(len == 48 + 20, "decs reply len=%zu want 68", len);
    static const uint8_t want_ver[20] = {
        0x00,0x00,0x00,0x14,  /* para atom_size 20 */
        'p','a','r','a',
        ',','v','e','r',
        '=','u','3','2',
        0x00,0x00,0x00,0x06,  /* value 6 */
    };
    CHECK(bytes_eq(buf + 48, want_ver, 20, ",ver =u32"), ",ver golden");

    /* =s64 ,frm = 0x0102030405060708 : 24 bytes. */
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, NULL, 101);
    aw_put_s64(&b, AW_KEY_FRM, 0x0102030405060708LL);
    aw_build_finalize(&b);
    static const uint8_t want_frm[24] = {
        0x00,0x00,0x00,0x18,  /* 24 */
        'p','a','r','a',
        ',','f','r','m',
        '=','s','6','4',
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    };
    CHECK(bytes_eq(buf + 48, want_frm, 24, ",frm =s64"), ",frm golden");

    /* =wxh ,fop-like dimensions 64 x 48. */
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, NULL, 102);
    aw_put_wxh(&b, AW_KEY_FOP, 64, 48);
    aw_build_finalize(&b);
    static const uint8_t want_wxh[24] = {
        0x00,0x00,0x00,0x18,
        'p','a','r','a',
        ',','f','o','p',
        '=','w','x','h',
        0x00,0x00,0x00,0x40,  /* 64 */
        0x00,0x00,0x00,0x30,  /* 48 */
    };
    CHECK(bytes_eq(buf + 48, want_wxh, 24, ",fop =wxh"), "=wxh golden");
}

/* ---- (1c) subs / rels golden + round-trip --------------------------- */

static void test_subs_rels(void)
{
    printf("-- subs / rels boxes --\n");
    uint8_t buf[128];

    /* subs: one used ID 0xAABBCCDD11223344, rest zero -> exactly 0x30 bytes. */
    uint64_t sids[AW_SUBS_IDS] = { 0xAABBCCDD11223344ULL, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(buf, sizeof buf, sids);
    CHECK(sl == 0x30, "subs len=%zu want 0x30", sl);
    static const uint8_t want_subs[16] = {
        0x00,0x00,0x00,0x30,  /* size 0x30 */
        's','u','b','s',
        0xAA,0xBB,0xCC,0xDD,0x11,0x22,0x33,0x44, /* id0 BE */
    };
    CHECK(bytes_eq(buf, want_subs, 16, "subs head"), "subs golden");
    uint64_t rb[AW_SUBS_IDS];
    CHECK(aw_subs_read(buf, sl, rb) == AW_PARSE_OK, "subs read");
    CHECK(rb[0] == sids[0] && rb[1] == 0, "subs round-trip");
    /* wrong-size subs rejected (delivered with no surfaces, per contract). */
    CHECK(aw_subs_read(buf, 0x20, rb) == AW_PARSE_BAD, "short subs rejected");

    /* rels: [u32 8+8n]['rels'][ids...]; zero IDs skipped on read. */
    uint64_t relids[3] = { 0x1111ULL, 0, 0x2222ULL };
    size_t rl = aw_rels_write(buf, sizeof buf, relids, 3);
    CHECK(rl == 8 + 24, "rels len=%zu want 32", rl);
    static const uint8_t want_rels[12] = {
        0x00,0x00,0x00,0x20,  /* 8 + 24 = 32 */
        'r','e','l','s',
        0x00,0x00,0x00,0x00,  /* id0 high word of 0x1111 */
    };
    CHECK(bytes_eq(buf, want_rels, 12, "rels head"), "rels golden");
    uint64_t rout[8]; unsigned n = 0;
    CHECK(aw_rels_read(buf, rl, rout, 8, &n) == AW_PARSE_OK, "rels read");
    CHECK(n == 2, "rels skipped zero: n=%u want 2", n);
    CHECK(rout[0] == 0x1111ULL && rout[1] == 0x2222ULL, "rels values");
}

/* ---- (2a) parse + scan rules (round-trip + Apple's refusals) -------- */

static void test_parse_scan(void)
{
    printf("-- parse + scan rules --\n");
    uint8_t buf[512];
    aw_builder b;
    uint8_t uuid[16];
    for (int i = 0; i < 16; i++) uuid[i] = (uint8_t)(0xE0 + i);

    /* Build a dfrm: ,frm (=s64), ,sbf (=sbf bytes), ,dfl (=u32 async). */
    static const uint8_t sbf[5] = { 's','b','u','f','?' };
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, 123);
    aw_put_s64(&b, AW_KEY_FRM, -9);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sizeof sbf);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    size_t len = aw_build_finalize(&b);
    CHECK(len > 48, "dfrm built");

    aw_msg_view v;
    CHECK(aw_parse(buf, len, &v) == AW_PARSE_OK, "parse ok");
    CHECK(v.type == AW_MSG_DFRM, "type dfrm");
    CHECK(v.flags == AW_FLAG_GUEST_REQ, "flags");
    CHECK(v.reply_id == 123, "reply_id");
    CHECK(memcmp(v.uuid, uuid, 16) == 0, "uuid round-trip");

    int64_t frm = 0;
    CHECK(aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK && frm == -9, "get ,frm");
    uint32_t dfl = 0;
    CHECK(aw_get_u32(&v, AW_KEY_DFL, &dfl) == AW_PARSE_OK && dfl == 1, "get ,dfl (async)");
    const uint8_t *sp; size_t slen;
    CHECK(aw_get_bytes(&v, AW_KEY_SBF, AW_VT_SBF, &sp, &slen) == AW_PARSE_OK &&
          slen == sizeof sbf && memcmp(sp, sbf, slen) == 0, "get ,sbf bytes");

    /* Scan rules (contract §2): */
    /* absent key -> ABSENT (reads as default, no error). */
    int32_t x;
    CHECK(aw_get_s32(&v, AW_KEY_COD, &x) == AW_PARSE_ABSENT, "absent ,cod -> ABSENT");
    /* right key, wrong value-type -> WRONGTYPE (the -19093 refusal). ,dfl was
     * written =u32; asking for =s32 must be rejected, as Apple rejects ,cod
     * sent =u32 when it reads =s32. */
    CHECK(aw_get_s32(&v, AW_KEY_DFL, &x) == AW_PARSE_WRONGTYPE, "wrong vtype -> WRONGTYPE");
}

static void test_parse_short_and_truncation(void)
{
    printf("-- malformed / short refusals --\n");
    /* A =u32 para whose atom claims 24 bytes (an =s64 length) is SHORT when
     * read as =u32. Hand-build it. */
    uint8_t buf[48 + 24];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DCAP, AW_FLAG_GUEST_REQ, NULL, 1);
    /* put ,cod with 8-byte payload but value-type =s32 (mismatched length). */
    aw_put_bytes(&b, AW_KEY_COD, AW_VT_S32, (const uint8_t *)"\0\0\0\0\0\0\0\0", 8);
    size_t len = aw_build_finalize(&b);
    aw_msg_view v;
    CHECK(aw_parse(buf, len, &v) == AW_PARSE_OK, "parse");
    int32_t cod;
    CHECK(aw_get_s32(&v, AW_KEY_COD, &cod) == AW_PARSE_SHORT, "bad length -> SHORT");

    /* A truncated message (atom claims more than present) is rejected. */
    uint8_t t[8] = { 0,0,0,48, 'v','t','p','v' }; /* claims 48, only 8 present */
    CHECK(aw_parse(t, sizeof t, &v) == AW_PARSE_BAD, "truncated vtpv -> BAD");
}

/* ---- (2b) parse skips a leading subs box ---------------------------- */

static void test_parse_skips_subs(void)
{
    printf("-- parse skips leading subs --\n");
    uint8_t buf[256];
    uint64_t ids[AW_SUBS_IDS] = { 7, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(buf, sizeof buf, ids);
    aw_builder b;
    aw_builder_init(&b, buf + sl, sizeof buf - sl);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_GUEST_REPLY, NULL, 55);
    uint8_t puuid[16]; memset(puuid, 0xAB, 16);
    aw_put_uui(&b, AW_KEY_UUI, puuid);
    size_t ml = aw_build_finalize(&b);

    aw_msg_view v;
    CHECK(aw_parse(buf, sl + ml, &v) == AW_PARSE_OK, "parse subs+vtpv");
    CHECK(v.hdr_off == sl, "vtpv found after subs (off=%zu want %zu)", v.hdr_off, sl);
    CHECK(v.type == AW_MSG_DBUF, "type Dbuf");
    uint8_t got[16];
    CHECK(aw_get_uui(&v, AW_KEY_UUI, got) == AW_PARSE_OK &&
          memcmp(got, puuid, 16) == 0, "uui param after subs");
    /* and the subs box reads the mapping ID the message lent. */
    uint64_t rids[AW_SUBS_IDS];
    CHECK(aw_subs_read(buf, sl, rids) == AW_PARSE_OK && rids[0] == 7, "subs id lent");
}

/* ---- (2c) async framing: flags + fragment reassembly ---------------- */

static void test_async_framing(void)
{
    printf("-- async framing + fragments --\n");
    uint8_t buf[128];
    aw_builder b;
    aw_msg_view v;

    /* A host->guest request (Dpba) carries AW_FLAG_HOST_REQ; its reply carries
     * AW_FLAG_GUEST_REPLY. An async dfrm reply carries HOST_REPLY. */
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DPBA, AW_FLAG_HOST_REQ, NULL, 200);
    aw_build_finalize(&b);
    CHECK(aw_parse(buf, b.len, &v) == AW_PARSE_OK, "parse Dpba");
    CHECK(v.flags == AW_FLAG_HOST_REQ, "Dpba is a host request");
    CHECK(!aw_more_fragments(&v), "Dpba not fragmented");

    /* Fragment reassembly: a large =sbf cut across two messages. The first is
     * flagged MORE_FRAGS with "expects reply" cleared; the last waits for the
     * reply (contract §5.3). A reader concatenates the keyed payloads. */
    uint8_t f0[128], f1[128];
    aw_builder b0, b1;

    aw_builder_init(&b0, f0, sizeof f0);
    aw_build_header(&b0, AW_MSG_DFRM, AW_FLAG_MORE_FRAGS, NULL, 300);
    aw_put_bytes(&b0, AW_KEY_SBF, AW_VT_SBF, (const uint8_t *)"HELLO", 5);
    size_t l0 = aw_build_finalize(&b0);

    aw_builder_init(&b1, f1, sizeof f1);
    aw_build_header(&b1, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, NULL, 300); /* last: expects reply */
    aw_put_bytes(&b1, AW_KEY_SBF, AW_VT_SBF, (const uint8_t *)"WORLD!", 6);
    size_t l1 = aw_build_finalize(&b1);

    aw_msg_view v0, v1;
    CHECK(aw_parse(f0, l0, &v0) == AW_PARSE_OK, "parse frag0");
    CHECK(aw_parse(f1, l1, &v1) == AW_PARSE_OK, "parse frag1");
    CHECK(aw_more_fragments(&v0), "frag0 MORE_FRAGS set");
    CHECK((v0.flags & AW_FLAG_GUEST_REQ) == 0, "frag0 does not expect a reply");
    CHECK(!aw_more_fragments(&v1), "frag1 MORE_FRAGS clear (last)");
    CHECK((v1.flags & AW_FLAG_GUEST_REQ) != 0, "frag1 expects the reply");

    uint8_t joined[32]; size_t jl = 0;
    const uint8_t *p; size_t pl;
    CHECK(aw_get_bytes(&v0, AW_KEY_SBF, AW_VT_SBF, &p, &pl) == AW_PARSE_OK, "frag0 ,sbf");
    memcpy(joined + jl, p, pl); jl += pl;
    CHECK(aw_get_bytes(&v1, AW_KEY_SBF, AW_VT_SBF, &p, &pl) == AW_PARSE_OK, "frag1 ,sbf");
    memcpy(joined + jl, p, pl); jl += pl;
    CHECK(jl == 11 && memcmp(joined, "HELLOWORLD!", 11) == 0, "fragments reassembled");
}

/* ---- (2d) builder overflow safety ----------------------------------- */

static void test_overflow(void)
{
    printf("-- builder overflow --\n");
    uint8_t small[40]; /* < 48, cannot hold even the header */
    aw_builder b;
    aw_builder_init(&b, small, sizeof small);
    CHECK(aw_build_header(&b, AW_MSG_DECS, 0, NULL, 0) == -1, "header overflow latches");
    CHECK(aw_build_finalize(&b) == 0, "finalize returns 0 on overflow");

    uint8_t ok[60];
    aw_builder_init(&b, ok, sizeof ok);
    aw_build_header(&b, AW_MSG_DECS, 0, NULL, 0);
    CHECK(aw_put_u32(&b, AW_KEY_VER, 6) == -1, "para past cap overflows");
    CHECK(aw_build_finalize(&b) == 0, "finalize 0 after para overflow");
}

int main(void)
{
    test_layout();
    test_golden_header();
    test_golden_para();
    test_subs_rels();
    test_parse_scan();
    test_parse_short_and_truncation();
    test_parse_skips_subs();
    test_async_framing();
    test_overflow();

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("OK: all aw_wire checks passed\n");
    return 0;
}
