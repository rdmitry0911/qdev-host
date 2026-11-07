/*
 * tb-vga plugin for qdev-host: hotpluggable stdvga-compatible display device
 * served to a stock QEMU through vfio-user-pci. Port of the qemu-thunderbolt
 * fork's "thunderbolt-vga" to an out-of-process device.
 *
 *   PCI 1234:1111 rev 2, subsystem 1af4:1100, class 03.00, PCIe endpoint
 *   BAR0  VRAM (default 16 MiB), 64-bit prefetchable, mmap-able memfd
 *   BAR2  4 KiB MMIO: 0x000 EDID, 0x400 VGA ports, 0x500 DISPI, 0x600 QEXT
 *
 * Plugin args: "edid=<file>,vram=<MiB>"; without edid= the EDID of the host
 * display (KMS sink) is used when the host provides one.
 * Display: get_scanout() exposes the VRAM memfd + current DISPI mode to the
 * host, which forwards it to Display1 listeners (QSM Direct), KMS or files.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include "qdp.h"
#include "pci_caps/px.h"

#define MMIO_SIZE   0x1000
#define EDID_MAX    0x400
#define VGA_OFF     0x400
#define VGA_SIZE    0x20
#define DISPI_OFF   0x500
#define DISPI_NR    0x0b
#define QEXT_OFF    0x600
#define PIXMAN_x8r8g8b8 0x20020888u

enum { IDX_ID, IDX_XRES, IDX_YRES, IDX_BPP, IDX_ENABLE, IDX_BANK,
       IDX_VIRT_WIDTH, IDX_VIRT_HEIGHT, IDX_X_OFFSET, IDX_Y_OFFSET,
       IDX_VIDEO_MEMORY_64K };

typedef struct tbvga {
    uint16_t dispi[DISPI_NR];
    uint8_t  vga[VGA_SIZE];
    uint32_t qext_byteorder;
    uint8_t  edid[EDID_MAX];
    size_t   edid_len;
    uint8_t *vram;
    size_t   vram_size;
    int      vram_fd;
} tbvga;

static tbvga *g;   /* libvfio-user region callbacks get no private pointer */

static void dispi_reset(tbvga *d)
{
    memset(d->dispi, 0, sizeof(d->dispi));
    d->dispi[IDX_ID] = 0xB0C5;
    d->dispi[IDX_VIDEO_MEMORY_64K] = d->vram_size / 65536;
    d->dispi[IDX_BPP] = 32;
    d->qext_byteorder = 0x1e1e1e1e;
    memset(d->vga, 0, sizeof(d->vga));
}

static ssize_t bar0_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{
    (void)c;
    if ((size_t)off + n > g->vram_size) return -1;
    if (w) memcpy(g->vram + off, buf, n); else memcpy(buf, g->vram + off, n);
    return n;
}

static ssize_t bar2_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{
    tbvga *d = g;
    (void)c;
    if (off < EDID_MAX) {
        if (w) return n;
        for (size_t i = 0; i < n; i++) {
            size_t a = off + i;
            buf[i] = (a < d->edid_len) ? d->edid[a] : 0;
        }
        return n;
    }
    if (off >= VGA_OFF && off < VGA_OFF + VGA_SIZE) {
        size_t a = off - VGA_OFF;
        if (n > VGA_SIZE - a) n = VGA_SIZE - a;
        if (w) memcpy(d->vga + a, buf, n); else memcpy(buf, d->vga + a, n);
        return n;
    }
    if (off >= DISPI_OFF && off < DISPI_OFF + DISPI_NR * 2) {
        unsigned idx = (off - DISPI_OFF) >> 1;
        if (n != 2 || (off & 1)) { if (!w) memset(buf, 0, n); return n; }
        if (w) {
            uint16_t v; memcpy(&v, buf, 2);
            switch (idx) {
            case IDX_ID: case IDX_VIDEO_MEMORY_64K: break;
            case IDX_ENABLE:
                d->dispi[idx] = v;
                if (v & 1) {
                    if (!d->dispi[IDX_VIRT_WIDTH]) d->dispi[IDX_VIRT_WIDTH] = d->dispi[IDX_XRES];
                    if (!d->dispi[IDX_VIRT_HEIGHT]) d->dispi[IDX_VIRT_HEIGHT] = d->dispi[IDX_YRES];
                    if (!(v & 0x80)) memset(d->vram, 0, d->vram_size);
                    fprintf(stderr, "tb-vga: mode %ux%u bpp=%u virt=%ux%u\n",
                            d->dispi[IDX_XRES], d->dispi[IDX_YRES], d->dispi[IDX_BPP],
                            d->dispi[IDX_VIRT_WIDTH], d->dispi[IDX_VIRT_HEIGHT]);
                }
                break;
            default: d->dispi[idx] = v; break;
            }
        } else {
            memcpy(buf, &d->dispi[idx], 2);
        }
        return n;
    }
    if (off >= QEXT_OFF && off < QEXT_OFF + 8) {
        uint32_t v = 0;
        if (off == QEXT_OFF) v = 8;
        if (off == QEXT_OFF + 4) {
            if (w) { memcpy(&v, buf, n < 4 ? n : 4); if (v == 0xbebebebe || v == 0x1e1e1e1e) d->qext_byteorder = v; return n; }
            v = d->qext_byteorder;
        }
        if (!w) memcpy(buf, &v, n < 4 ? n : 4);
        return n;
    }
    if (!w) memset(buf, 0, n);
    return n;
}

static int parse_args(const char *args, char *edid_path, size_t edid_sz, size_t *vram_mib)
{
    *vram_mib = 16; edid_path[0] = 0;
    if (!args) return 0;
    char *copy = strdup(args), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (!strncmp(tok, "edid=", 5)) snprintf(edid_path, edid_sz, "%s", tok + 5);
        else if (!strncmp(tok, "vram=", 5)) *vram_mib = strtoul(tok + 5, NULL, 10);
        else { fprintf(stderr, "tb-vga: unknown arg '%s'\n", tok); free(copy); return -EINVAL; }
    }
    free(copy);
    if (*vram_mib < 4 || *vram_mib > 256 || (*vram_mib & (*vram_mib - 1))) return -EINVAL;
    return 0;
}

static int tbvga_setup(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env, void **priv)
{
    char edid_path[512]; size_t vram_mib;
    int r = parse_args(args, edid_path, sizeof(edid_path), &vram_mib);
    if (r) return r;

    tbvga *d = calloc(1, sizeof(*d));
    d->vram_size = vram_mib << 20;
    if (edid_path[0]) {
        FILE *f = fopen(edid_path, "rb");
        if (!f) { fprintf(stderr, "tb-vga: cannot open %s\n", edid_path); free(d); return -errno; }
        d->edid_len = fread(d->edid, 1, sizeof(d->edid), f);
        fclose(f);
    } else if (env && env->edid && env->edid_len) {
        d->edid_len = env->edid_len > sizeof(d->edid) ? sizeof(d->edid) : env->edid_len;
        memcpy(d->edid, env->edid, d->edid_len);
        fprintf(stderr, "tb-vga: using the host display's EDID (%u bytes)\n", env->edid_len);
    }
    d->vram_fd = memfd_create("tb-vga-vram", 0);
    if (d->vram_fd < 0 || ftruncate(d->vram_fd, d->vram_size) < 0) { free(d); return -errno; }
    d->vram = mmap(NULL, d->vram_size, PROT_READ | PROT_WRITE, MAP_SHARED, d->vram_fd, 0);
    if (d->vram == MAP_FAILED) { close(d->vram_fd); free(d); return -errno; }
    dispi_reset(d);
    g = d;

    if (vfu_pci_init(ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 2) < 0) return -errno;
    vfu_pci_set_id(ctx, 0x1234, 0x1111, 0x1af4, 0x1100);
    vfu_pci_set_class(ctx, 0x03, 0x00, 0x00);
    struct pxcap px = {
        .hdr = { .id = PCI_CAP_ID_EXP },
        .pxcaps = { .ver = 2, .dpt = 0 },
        .pxdcap = { .mps = 1, .flrc = 1 },
        .pxlcap = { .stuff = 0x01000043 },   /* 8 GT/s, x4, port 1 */
        .pxls   = { .stuff = 0x0043 },
    };
    if (vfu_pci_add_capability(ctx, 0, 0, &px) < 0) return -errno;
    struct iovec vram_area = { .iov_base = 0, .iov_len = d->vram_size };
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, d->vram_size, bar0_cb,
                         VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM | VFU_REGION_FLAG_PREFETCH |
                         VFU_REGION_FLAG_64_BITS, &vram_area, 1, d->vram_fd, 0) < 0) return -errno;
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR2_REGION_IDX, MMIO_SIZE, bar2_cb,
                         VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0) < 0) return -errno;
    *priv = d;
    fprintf(stderr, "tb-vga: VRAM %zu MiB, EDID %zu bytes\n", vram_mib, d->edid_len);
    return 0;
}

static void tbvga_teardown(void *priv)
{
    tbvga *d = priv;
    if (!d) return;
    munmap(d->vram, d->vram_size); close(d->vram_fd); free(d);
    if (g == d) g = NULL;
}

static int tbvga_reset(void *priv, vfu_reset_type_t type)
{
    tbvga *d = priv;
    if (type != VFU_RESET_LOST_CONN) dispi_reset(d);   /* keep the mode over a lost connection */
    return 0;
}

static bool tbvga_get_scanout(void *priv, qdp_scanout *out)
{
    tbvga *d = priv;
    uint32_t w = d->dispi[IDX_XRES], h = d->dispi[IDX_YRES];
    uint32_t vw = d->dispi[IDX_VIRT_WIDTH] ? d->dispi[IDX_VIRT_WIDTH] : w;
    bool on = (d->dispi[IDX_ENABLE] & 1) && d->dispi[IDX_BPP] == 32 && w && h;
    memset(out, 0, sizeof(*out));
    out->fd = d->vram_fd;
    if (!on) return true;
    out->offset = ((uint32_t)d->dispi[IDX_Y_OFFSET] * vw + d->dispi[IDX_X_OFFSET]) * 4;
    out->width = w; out->height = h; out->stride = vw * 4;
    out->pixman_format = PIXMAN_x8r8g8b8;
    if ((size_t)out->offset + (size_t)out->stride * h > d->vram_size) return true;  /* bogus mode: stay disabled */
    out->enabled = true;
    return true;
}

static const qdp_device_ops ops = {
    .abi_version = QDP_ABI_VERSION,
    .name = "tb-vga",
    .version = "0.2",
    .setup = tbvga_setup,
    .teardown = tbvga_teardown,
    .reset = tbvga_reset,
    .get_scanout = tbvga_get_scanout,
};

const qdp_device_ops *qdp_plugin_v1(void) { return &ops; }
