/* test_lavc_hevc10.c - the HEVC Main 10 admission knob (D-GVA-P010, plugin `hevc10=0|1`) on the
 * production stream core (software device: it decodes Main 10, as NVDEC and VA-API radeonsi do).
 *  KNOB-01: hevc10 off -> a Main 10 record is refused at open with VTPV_ERR_UNSUPPORTED, the code
 *           main ba41a66 returns for it (record_is_8bit_420) -> the same dsta refusal -> the guest's
 *           software decoder, as before P010; an 8-bit HEVC record still opens with the '420v' pool.
 *  KNOB-02: hevc10 on -> the Main 10 record opens with the 'x420' pool and decodes to P010.
 *  KNOB-03: set_hevc10 reports the effective state (on only where the device decodes Main 10).
 * Exit 77 = no libx265 encoder here. */
#include <stdio.h>
#include <string.h>
#include "vtpv_lavc.h"
#include "test_lavc_util.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    stream m10, m8;
    vtpv_lavc *v = NULL;
    vtpv_lavc_dec *d = NULL;
    vtpv_lavc_frame f;
    int rc = make_stream("libx265", "bframes=0:keyint=4:log-level=error", AV_PIX_FMT_YUV420P10LE, 320, 240, 2, "k10", &m10);
    if (rc == 77) { printf("SKIP: no libx265\n"); return 77; }
    CHECK(rc == 0, "Main 10 stream");
    CHECK(make_stream("libx265", "bframes=0:keyint=4:log-level=error", AV_PIX_FMT_YUV420P, 320, 240, 2, "k8", &m8) == 0, "Main stream");
    if (g_fail) return 1;
    CHECK(vtpv_lavc_open(VTPV_LAVC_HW_NONE, NULL, &v, NULL, 0) == VTPV_OK, "open core");

    /* KNOB-01 */
    CHECK(vtpv_lavc_set_hevc10(v, 0) == 0, "off: effective state 0");
    rc = vtpv_lavc_open_decoder(v, VTPV_CODEC_HEVC, 320, 240, m10.extradata, m10.extradata_len, &d);
    CHECK(rc == VTPV_ERR_UNSUPPORTED && !d, "off: Main 10 refused UNSUPPORTED (rc %d)", rc);
    rc = vtpv_lavc_open_decoder(v, VTPV_CODEC_HEVC, 320, 240, m8.extradata, m8.extradata_len, &d);
    CHECK(rc == VTPV_OK && d && vtpv_lavc_output_format(d) == VTPV_CV_420V, "off: 8-bit HEVC opens, '420v' (rc %d)", rc);
    if (d) { vtpv_lavc_close_decoder(d); d = NULL; }

    /* KNOB-02 / 03 */
    CHECK(vtpv_lavc_set_hevc10(v, 1) == 1, "on: effective state 1 (the software device decodes Main 10)");
    rc = vtpv_lavc_open_decoder(v, VTPV_CODEC_HEVC, 320, 240, m10.extradata, m10.extradata_len, &d);
    CHECK(rc == VTPV_OK && d && vtpv_lavc_output_format(d) == VTPV_CV_X420, "on: Main 10 opens, 'x420' (rc %d)", rc);
    if (d) {
        rc = vtpv_lavc_decode(d, m10.pkt[0], m10.pkt_len[0], &f);
        CHECK(rc == VTPV_OK && f.pixfmt == VTPV_PIXFMT_P010 && f.width == 320 && f.height == 240, "on: decodes to P010 (rc %d)", rc);
        vtpv_lavc_close_decoder(d);
    }
    vtpv_lavc_close(v);
    stream_free(&m10); stream_free(&m8);
    if (g_fail) { printf("FAILED: %d of %d checks\n", g_fail, g_checks); return 1; }
    printf("OK: %d hevc10 knob checks passed\n", g_checks);
    return 0;
}
