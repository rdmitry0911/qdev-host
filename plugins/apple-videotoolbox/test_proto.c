/*
 * test_proto.c - unit tests for the AppleGVA codec-session protocol handler.
 * No libva: a mock decoder backend drives submit->dispatch->complete.
 * CLEAN-ROOM. Increment 1, lane L-M2V03, 2026-11-16.
 *
 * Build/run: see the module Makefile target `test-proto`.
 */
#include "vtpv_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

/* ---- captured completions ------------------------------------------ */

#define MAXC 32
static uint8_t  g_cmpl[MAXC][512];
static size_t   g_cmpl_len[MAXC];
static int      g_ncmpl;

static void sink(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (g_ncmpl < MAXC && len <= sizeof(g_cmpl[0])) {
        memcpy(g_cmpl[g_ncmpl], buf, len);
        g_cmpl_len[g_ncmpl] = len;
    }
    g_ncmpl++;
}
static void sink_reset(void) { g_ncmpl = 0; }

static const vtpv_cmpl_hdr *last_hdr(void)
{
    return (g_ncmpl >= 1 && g_ncmpl <= MAXC)
        ? (const vtpv_cmpl_hdr *)g_cmpl[g_ncmpl - 1] : NULL;
}

/* ---- a mock decoder backend ---------------------------------------- */

typedef struct mock_dec { uint32_t codec, w, h; } mock_dec;
static int    g_open_calls, g_decode_calls, g_close_calls;
static size_t g_last_sample_len;
static int    g_open_should_fail;

static int mock_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    vtpv_codec_cap *c = (vtpv_codec_cap *)((uint8_t *)out + sizeof(*out));
    (void)ctx;
    if (cap < sizeof(*out) + 2 * sizeof(*c)) return -1;
    out->magic = VTPV_CAPS_MAGIC; out->abi = VTPV_ABI; out->n_codecs = 2;
    c[0].codec = VTPV_CODEC_H264; c[0].can_decode = 1; c[0].can_encode = 0;
    c[0].reserved = 0; c[0].max_width = 4096; c[0].max_height = 4096;
    c[0].out_pixfmt = VTPV_PIXFMT_NV12;
    c[1].codec = VTPV_CODEC_HEVC; c[1].can_decode = 1; c[1].can_encode = 0;
    c[1].reserved = 0; c[1].max_width = 8192; c[1].max_height = 8192;
    c[1].out_pixfmt = VTPV_PIXFMT_NV12;
    return 2;
}
static int mock_open(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                     const uint8_t *ed, size_t ed_len, void **dec)
{
    mock_dec *d;
    (void)ctx; (void)ed; (void)ed_len;
    g_open_calls++;
    if (g_open_should_fail) return VTPV_ERR_BACKEND;
    d = calloc(1, sizeof(*d));
    d->codec = codec; d->w = w; d->h = h;
    *dec = d;
    return VTPV_OK;
}
static int mock_decode(void *dec, const uint8_t *s, size_t slen,
                       vtpv_decoded_frame *out)
{
    mock_dec *d = dec;
    (void)s;
    g_decode_calls++;
    g_last_sample_len = slen;
    out->surface_id = 0x5100 + g_decode_calls;
    out->width = d->w; out->height = d->h; out->pixfmt = VTPV_PIXFMT_NV12;
    return VTPV_OK;
}
static void mock_close(void *dec) { g_close_calls++; free(dec); }

static const vtpv_decoder_backend mock_be = {
    mock_query_caps, mock_open, mock_decode, mock_close, NULL, NULL
};

/* ---- message builders ---------------------------------------------- */

static size_t build_create(uint8_t *buf, uint32_t codec, uint32_t w, uint32_t h,
                           const uint8_t *ed, uint32_t ed_len, uint64_t tag)
{
    vtpv_submit_hdr *hd = (vtpv_submit_hdr *)buf;
    vtpv_create_req *rq = (vtpv_create_req *)(buf + sizeof(*hd));
    size_t len = sizeof(*hd) + sizeof(*rq) + ed_len;
    memset(hd, 0, sizeof(*hd));
    hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
    hd->type = VTPV_MSG_SESSION_CREATE; hd->session = 0; hd->tag = tag;
    hd->length = (uint32_t)len;
    rq->codec = codec; rq->width = w; rq->height = h; rq->extradata_len = ed_len;
    if (ed_len && ed) memcpy(buf + sizeof(*hd) + sizeof(*rq), ed, ed_len);
    return len;
}
static size_t build_decode(uint8_t *buf, uint32_t session, uint64_t pts,
                           uint32_t sample_len, uint64_t tag)
{
    vtpv_submit_hdr *hd = (vtpv_submit_hdr *)buf;
    vtpv_decode_req *rq = (vtpv_decode_req *)(buf + sizeof(*hd));
    size_t len = sizeof(*hd) + sizeof(*rq) + sample_len;
    memset(hd, 0, sizeof(*hd));
    hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
    hd->type = VTPV_MSG_DECODE_FRAME; hd->session = session; hd->tag = tag;
    hd->length = (uint32_t)len;
    rq->pts = pts; rq->flags = 0; rq->sample_len = sample_len;
    memset(buf + sizeof(*hd) + sizeof(*rq), 0xA5, sample_len);
    return len;
}
static size_t build_simple(uint8_t *buf, uint16_t type, uint32_t session,
                           uint64_t tag)
{
    vtpv_submit_hdr *hd = (vtpv_submit_hdr *)buf;
    memset(hd, 0, sizeof(*hd));
    hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
    hd->type = type; hd->session = session; hd->tag = tag;
    hd->length = sizeof(*hd);
    return sizeof(*hd);
}

/* ---- tests --------------------------------------------------------- */

int main(void)
{
    uint8_t buf[VTPV_MAX_SAMPLE_BYTES + 256];
    vtpv_proto *p;
    size_t n;
    int rc;
    uint32_t sid = 0;

    printf("apple-videotoolbox: protocol handler unit tests\n");

    p = vtpv_proto_create(&mock_be, NULL, sink, NULL);
    CHECK(p != NULL, "proto_create");

    /* 1. QUERY_CAPS -> a D-GVA-2 caps report with 2 codecs. */
    sink_reset();
    n = build_simple(buf, VTPV_MSG_QUERY_CAPS, 0, 0x11);
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc == 0 && g_ncmpl == 1, "caps: one completion (rc=%d n=%d)", rc, g_ncmpl);
    {
        const vtpv_cmpl_hdr *h = last_hdr();
        const vtpv_caps *caps = (const vtpv_caps *)(g_cmpl[0] + sizeof(*h));
        CHECK(h && h->status == VTPV_OK && h->tag == 0x11, "caps: ok status+tag");
        CHECK(caps->magic == VTPV_CAPS_MAGIC && caps->n_codecs == 2,
              "caps: magic + 2 codecs (n=%u)", caps ? caps->n_codecs : 0);
    }

    /* 2. SESSION_CREATE H.264 -> ok + a session id. */
    sink_reset();
    {
        uint8_t avcc[8] = { 0x01, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 0 };
        n = build_create(buf, VTPV_CODEC_H264, 320, 240, avcc, sizeof(avcc), 0x22);
        rc = vtpv_proto_submit(p, buf, n);
        const vtpv_cmpl_hdr *h = last_hdr();
        const vtpv_create_desc *d =
            (const vtpv_create_desc *)(g_cmpl[0] + sizeof(*h));
        CHECK(rc == 0 && g_ncmpl == 1 && h && h->status == VTPV_OK,
              "create: ok (rc=%d status=%d)", rc, h ? h->status : 99);
        CHECK(d && d->session != 0, "create: nonzero session id");
        CHECK(g_open_calls == 1, "create: backend open called once (%d)", g_open_calls);
        sid = d ? d->session : 0;
        CHECK(vtpv_proto_open_sessions(p) == 1, "create: one open session");
    }

    /* 3. DECODE_FRAME on that session -> ok + surface descriptor. */
    sink_reset();
    n = build_decode(buf, sid, 1000, 4096, 0x33);
    rc = vtpv_proto_submit(p, buf, n);
    {
        const vtpv_cmpl_hdr *h = last_hdr();
        const vtpv_decoded_desc *d =
            (const vtpv_decoded_desc *)(g_cmpl[0] + sizeof(*h));
        CHECK(rc == 0 && h && h->status == VTPV_OK, "decode: ok");
        CHECK(h && h->tag == 0x33 && h->session == sid, "decode: echoes tag+session");
        CHECK(d && d->pts == 1000 && d->pixfmt == VTPV_PIXFMT_NV12 &&
              d->width == 320 && d->height == 240, "decode: surface descriptor");
        CHECK(g_last_sample_len == 4096, "decode: sample reached backend (%zu)",
              g_last_sample_len);
    }

    /* 4. DECODE_FRAME on an unknown session -> NO_SESSION. */
    sink_reset();
    n = build_decode(buf, 0xdead, 0, 16, 0x34);
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_NO_SESSION,
          "decode: unknown session rejected");

    /* 5. DECODE_FRAME over the 1 MiB sample limit -> TOO_LARGE, no backend. */
    sink_reset();
    {
        int before = g_decode_calls;
        vtpv_submit_hdr *hd = (vtpv_submit_hdr *)buf;
        vtpv_decode_req *rq = (vtpv_decode_req *)(buf + sizeof(*hd));
        memset(hd, 0, sizeof(*hd));
        hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
        hd->type = VTPV_MSG_DECODE_FRAME; hd->session = sid; hd->tag = 0x35;
        hd->length = sizeof(*hd) + sizeof(*rq); /* claim no sample bytes present */
        rq->pts = 0; rq->flags = 0; rq->sample_len = VTPV_MAX_SAMPLE_BYTES + 1;
        rc = vtpv_proto_submit(p, buf, sizeof(*hd) + sizeof(*rq));
        CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_TOO_LARGE,
              "decode: >1MiB sample refused");
        CHECK(g_decode_calls == before, "decode: oversize never hit backend");
    }

    /* 6. SESSION_CREATE unsupported codec -> UNSUPPORTED. */
    sink_reset();
    n = build_create(buf, 0x766f6964 /*'void'*/, 320, 240, NULL, 0, 0x36);
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_UNSUPPORTED,
          "create: unsupported codec refused");

    /* 7. SESSION_CREATE oversized extradata -> TOO_LARGE. */
    sink_reset();
    {
        vtpv_submit_hdr *hd = (vtpv_submit_hdr *)buf;
        vtpv_create_req *rq = (vtpv_create_req *)(buf + sizeof(*hd));
        memset(hd, 0, sizeof(*hd));
        hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
        hd->type = VTPV_MSG_SESSION_CREATE; hd->tag = 0x37;
        hd->length = sizeof(*hd) + sizeof(*rq);
        rq->codec = VTPV_CODEC_H264; rq->width = 320; rq->height = 240;
        rq->extradata_len = VTPV_MAX_EXTRADATA_BYTES + 1;
        rc = vtpv_proto_submit(p, buf, sizeof(*hd) + sizeof(*rq));
        CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_TOO_LARGE,
              "create: oversized extradata refused");
    }

    /* 8. SESSION_END -> ok + backend close; session gone. */
    sink_reset();
    n = build_simple(buf, VTPV_MSG_SESSION_END, sid, 0x38);
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_OK, "end: ok");
    CHECK(g_close_calls == 1, "end: backend close called (%d)", g_close_calls);
    CHECK(vtpv_proto_open_sessions(p) == 0, "end: no sessions left");

    /* 9. SESSION_END on a closed session -> NO_SESSION. */
    sink_reset();
    n = build_simple(buf, VTPV_MSG_SESSION_END, sid, 0x39);
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_NO_SESSION,
          "end: double-end refused");

    /* 10. Malformed: bad magic -> no completion, rc<0. */
    sink_reset();
    n = build_simple(buf, VTPV_MSG_QUERY_CAPS, 0, 0x40);
    ((vtpv_submit_hdr *)buf)->magic = 0xdeadbeef;
    rc = vtpv_proto_submit(p, buf, n);
    CHECK(rc < 0 && g_ncmpl == 0, "malformed: bad magic dropped, no completion");

    /* 11. Truncated: header claims more than we pass -> MALFORMED completion. */
    sink_reset();
    n = build_decode(buf, sid, 0, 4096, 0x41);
    rc = vtpv_proto_submit(p, buf, n - 100); /* pass fewer bytes than length */
    CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_MALFORMED,
          "truncated: length>received refused");

    /* 12. Session-table exhaustion -> NO_SLOTS after VTPV_MAX_SESSIONS. */
    sink_reset();
    {
        unsigned i; int got_no_slots = 0;
        uint8_t avcc[7] = { 0x01, 0x64, 0, 0x1f, 0xff, 0xe1, 0 };
        for (i = 0; i < VTPV_MAX_SESSIONS + 2; i++) {
            size_t m = build_create(buf, VTPV_CODEC_H264, 64, 64, avcc, sizeof(avcc), 0x50 + i);
            sink_reset(); /* keep last_hdr() valid across the long loop */
            vtpv_proto_submit(p, buf, m);
            if (last_hdr() && last_hdr()->status == VTPV_ERR_NO_SLOTS) got_no_slots = 1;
        }
        CHECK(got_no_slots, "exhaustion: NO_SLOTS once the table fills");
        CHECK(vtpv_proto_open_sessions(p) == VTPV_MAX_SESSIONS,
              "exhaustion: exactly MAX sessions open (%u)", vtpv_proto_open_sessions(p));
    }

    /* 13. open_decoder failure path frees the session slot. */
    sink_reset();
    vtpv_proto_destroy(p);
    p = vtpv_proto_create(&mock_be, NULL, sink, NULL);
    g_open_should_fail = 1;
    {
        uint8_t avcc[7] = { 0x01, 0x64, 0, 0x1f, 0xff, 0xe1, 0 };
        n = build_create(buf, VTPV_CODEC_H264, 64, 64, avcc, sizeof(avcc), 0x60);
        rc = vtpv_proto_submit(p, buf, n);
        CHECK(rc == 0 && last_hdr() && last_hdr()->status == VTPV_ERR_BACKEND,
              "open-fail: backend error surfaced");
        CHECK(vtpv_proto_open_sessions(p) == 0, "open-fail: slot released");
    }
    g_open_should_fail = 0;
    vtpv_proto_destroy(p);

    printf("%s: protocol handler (%s)\n", g_fail ? "FAIL" : "PASS",
           g_fail ? "failures above" : "13 cases");
    return g_fail ? 1 : 0;
}
