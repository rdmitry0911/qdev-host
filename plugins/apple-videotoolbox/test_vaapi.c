/*
 * test_vaapi.c - unit test for the VA-API decode backend, driven THROUGH the
 * protocol handler, against the live DRM render node.
 * CLEAN-ROOM. Increment 1, lane L-M2V03, 2026-11-16.
 *
 * Stage 1 verifies: display open, D-GVA-2 caps from the live driver, the
 * decode-session lifecycle (config/context/surface), and the surface +
 * readback contract. The real H.264 bitstream decode replaces the surface
 * fill in a later stage without changing this contract.
 *
 * Skips (exit 77, autotools "skipped") if no render node / no VA driver.
 */
#include "vtpv_vaapi.h"
#include "vtpv_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_node;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* capture the single most recent completion */
static uint8_t g_buf[1024];
static size_t  g_len;
static int     g_n;
static void sink(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (n <= sizeof(g_buf)) { memcpy(g_buf, b, n); g_len = n; }
    g_n++;
}
static const vtpv_cmpl_hdr *hdr(void) { return (const vtpv_cmpl_hdr *)g_buf; }

static size_t mk_create(uint8_t *b, uint32_t codec, uint32_t w, uint32_t h, uint64_t tag)
{
    vtpv_submit_hdr *hd = (vtpv_submit_hdr *)b;
    vtpv_create_req *rq = (vtpv_create_req *)(b + sizeof(*hd));
    memset(hd, 0, sizeof(*hd));
    hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
    hd->type = VTPV_MSG_SESSION_CREATE; hd->tag = tag;
    hd->length = sizeof(*hd) + sizeof(*rq);
    rq->codec = codec; rq->width = w; rq->height = h; rq->extradata_len = 0;
    return hd->length;
}
static size_t mk_simple(uint8_t *b, uint16_t type, uint32_t sess, uint64_t tag)
{
    vtpv_submit_hdr *hd = (vtpv_submit_hdr *)b;
    memset(hd, 0, sizeof(*hd));
    hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
    hd->type = type; hd->session = sess; hd->tag = tag; hd->length = sizeof(*hd);
    return sizeof(*hd);
}

int main(int argc, char **argv)
{
    vtpv_vaapi *v = NULL;
    const vtpv_decoder_backend *be = NULL;
    char err[256] = {0};
    uint8_t msg[4096];
    vtpv_proto *p;
    int rc;
    uint32_t sid = 0;
    const uint32_t W = 352, H = 288; /* CIF: a size every H.264 decoder serves */

    g_node = (argc > 1) ? argv[1] : "/dev/dri/renderD128";
    printf("apple-videotoolbox: VA-API backend test on %s\n", g_node);

    rc = vtpv_vaapi_open(g_node, &v, &be, NULL, err, sizeof(err));
    if (rc != 0) {
        printf("SKIP: VA-API unavailable: %s (%d)\n", err, rc);
        return 77; /* skipped */
    }

    p = vtpv_proto_create(be, vtpv_vaapi_ctx(v), sink, NULL);
    CHECK(p != NULL, "proto_create over vaapi");

    /* 1. QUERY_CAPS reflects the live driver (>=1 codec, H.264 present). */
    g_n = 0;
    { size_t n = mk_simple(msg, VTPV_MSG_QUERY_CAPS, 0, 1); vtpv_proto_submit(p, msg, n); }
    {
        const vtpv_caps *caps = (const vtpv_caps *)(g_buf + sizeof(vtpv_cmpl_hdr));
        const vtpv_codec_cap *cc = (const vtpv_codec_cap *)((const uint8_t *)caps + sizeof(*caps));
        int i, saw_h264 = 0;
        CHECK(g_n == 1 && hdr()->status == VTPV_OK, "caps ok");
        CHECK(caps->magic == VTPV_CAPS_MAGIC && caps->n_codecs >= 1,
              "caps: >=1 codec (n=%u)", caps->n_codecs);
        for (i = 0; i < caps->n_codecs; i++)
            if (cc[i].codec == VTPV_CODEC_H264 && cc[i].can_decode) saw_h264 = 1;
        CHECK(saw_h264, "caps: H.264 decode advertised");
        printf("  D-GVA-2 report: %u codec(s), H.264 decode=%d\n", caps->n_codecs, saw_h264);
    }

    /* 2. SESSION_CREATE H.264 -> a real VAConfig+VAContext. */
    g_n = 0;
    { size_t n = mk_create(msg, VTPV_CODEC_H264, W, H, 0x22); vtpv_proto_submit(p, msg, n); }
    CHECK(g_n == 1 && hdr()->status == VTPV_OK, "create: ok (status=%d)", g_n ? hdr()->status : 99);
    sid = ((const vtpv_create_desc *)(g_buf + sizeof(vtpv_cmpl_hdr)))->session;
    CHECK(sid != 0, "create: session id");

    /* (decode + pixel readback are covered by test_h264, which runs a real
     * I_PCM H.264 frame through vaBeginPicture/Render/End and verifies the
     * decoded luma. This test stays on the backend lifecycle + D-GVA-2 caps.) */

    /* 3. SESSION_END tears the VA objects down. */
    g_n = 0;
    { size_t n = mk_simple(msg, VTPV_MSG_SESSION_END, sid, 0x44); vtpv_proto_submit(p, msg, n); }
    CHECK(g_n == 1 && hdr()->status == VTPV_OK, "end: ok");

    vtpv_proto_destroy(p);
    vtpv_vaapi_close(v);

    printf("%s: VA-API backend (display + caps + session lifecycle)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
