/*
 * test_h264.c - stage-2 unit test: a real H.264 decode on the live driver.
 * Generates a minimal I_PCM I-frame (known pixels), decodes it through the
 * protocol handler + VA-API backend, and verifies the decoded surface luma
 * equals the samples that were encoded. CLEAN-ROOM. Increment 1 stage 2,
 * lane L-M2V03, 2026-11-16.
 *
 * exit 0 PASS, 1 FAIL, 77 skipped (no render node / backend).
 */
#include "vtpv_vaapi.h"
#include "vtpv_proto.h"
#include "h264_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_cmpl[1024];
static int     g_n;
static void sink(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (n <= sizeof(g_cmpl)) memcpy(g_cmpl, b, n);
    g_n++;
}
static const vtpv_cmpl_hdr *hdr(void) { return (const vtpv_cmpl_hdr *)g_cmpl; }

int main(int argc, char **argv)
{
    const char *node = (argc > 1) ? argv[1] : "/dev/dri/renderD128";
    vtpv_vaapi *v = NULL;
    const vtpv_decoder_backend *be = NULL;
    char err[256] = {0};
    const int W = 352, H = 288;        /* a size radeonsi serves for H.264 */
    vtpv_proto *p;
    uint8_t *bitstream, *luma_ref, *luma_out, *msg;
    size_t bs_len, msg_len;
    uint32_t sid, surface_id;
    int rc;

    printf("apple-videotoolbox: H.264 I_PCM decode test on %s (%dx%d)\n", node, W, H);

    rc = vtpv_vaapi_open(node, &v, &be, NULL, err, sizeof(err));
    if (rc != 0) { printf("SKIP: VA-API unavailable: %s (%d)\n", err, rc); return 77; }

    bitstream = malloc(1u << 20);
    luma_ref  = malloc((size_t)W * H);
    luma_out  = malloc((size_t)W * H);
    msg       = malloc((1u << 20) + 256);
    if (!bitstream || !luma_ref || !luma_out || !msg) { printf("OOM\n"); return 1; }

    bs_len = h264_gen_ipcm_frame(W, H, bitstream, 1u << 20, luma_ref);
    CHECK(bs_len > 0 && bs_len < (1u << 20), "generated I_PCM bitstream (%zu bytes)", bs_len);
    if (bs_len == 0) { printf("FAIL: generator\n"); return 1; }

    p = vtpv_proto_create(be, vtpv_vaapi_ctx(v), sink, NULL);

    /* SESSION_CREATE H.264 */
    {
        vtpv_submit_hdr *hd = (vtpv_submit_hdr *)msg;
        vtpv_create_req *rq = (vtpv_create_req *)(msg + sizeof(*hd));
        memset(hd, 0, sizeof(*hd));
        hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
        hd->type = VTPV_MSG_SESSION_CREATE; hd->tag = 1;
        hd->length = sizeof(*hd) + sizeof(*rq);
        rq->codec = VTPV_CODEC_H264; rq->width = W; rq->height = H; rq->extradata_len = 0;
        g_n = 0;
        vtpv_proto_submit(p, msg, hd->length);
        CHECK(g_n == 1 && hdr()->status == VTPV_OK, "create: ok (status=%d)",
              g_n ? hdr()->status : 99);
        sid = ((const vtpv_create_desc *)(g_cmpl + sizeof(vtpv_cmpl_hdr)))->session;
        CHECK(sid != 0, "create: session id");
    }

    /* DECODE_FRAME with the I_PCM bitstream */
    {
        vtpv_submit_hdr *hd = (vtpv_submit_hdr *)msg;
        vtpv_decode_req *rq = (vtpv_decode_req *)(msg + sizeof(*hd));
        memset(hd, 0, sizeof(*hd));
        hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
        hd->type = VTPV_MSG_DECODE_FRAME; hd->session = sid; hd->tag = 2;
        rq->pts = 0; rq->flags = 0; rq->sample_len = (uint32_t)bs_len;
        memcpy(msg + sizeof(*hd) + sizeof(*rq), bitstream, bs_len);
        msg_len = sizeof(*hd) + sizeof(*rq) + bs_len;
        hd->length = (uint32_t)msg_len;
        g_n = 0;
        vtpv_proto_submit(p, msg, msg_len);
        CHECK(g_n == 1 && hdr()->status == VTPV_OK, "decode: ok (status=%d)",
              g_n ? hdr()->status : 99);
        surface_id = ((const vtpv_decoded_desc *)(g_cmpl + sizeof(vtpv_cmpl_hdr)))->surface_id;
        CHECK(surface_id != 0, "decode: surface id");
    }

    /* Verify the decoded luma equals the samples we encoded. */
    if (g_fail == 0) {
        memset(luma_out, 0, (size_t)W * H);
        rc = vtpv_vaapi_read_luma(v, surface_id, luma_out, W, W, H);
        CHECK(rc == 0, "readback: luma (rc=%d)", rc);
        if (rc == 0) {
            size_t bad = 0, first = 0; int x, y;
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    if (luma_out[y * W + x] != luma_ref[y * W + x]) {
                        if (!bad) first = (size_t)y * W + x;
                        bad++;
                    }
            printf("  luma compare: %zu/%d mismatches; pixel0 got=%u want=%u\n",
                   bad, W * H, luma_out[0], luma_ref[0]);
            if (bad)
                printf("  first mismatch at (%d,%d) got=%u want=%u\n",
                       (int)(first % W), (int)(first / W),
                       luma_out[first], luma_ref[first]);
            CHECK(bad == 0, "decoded luma matches the encoded I_PCM samples");
        }
    }

    /* SESSION_END */
    {
        vtpv_submit_hdr *hd = (vtpv_submit_hdr *)msg;
        memset(hd, 0, sizeof(*hd));
        hd->magic = VTPV_SUBMIT_MAGIC; hd->abi = VTPV_ABI;
        hd->type = VTPV_MSG_SESSION_END; hd->session = sid; hd->tag = 3;
        hd->length = sizeof(*hd);
        g_n = 0;
        vtpv_proto_submit(p, msg, sizeof(*hd));
        CHECK(g_n == 1 && hdr()->status == VTPV_OK, "end: ok");
    }

    vtpv_proto_destroy(p);
    vtpv_vaapi_close(v);
    free(bitstream); free(luma_ref); free(luma_out); free(msg);

    printf("%s: H.264 I_PCM decode (real vaBeginPicture/Render/End + pixel verify)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
