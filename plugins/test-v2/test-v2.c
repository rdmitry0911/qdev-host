/*
 * test-v2 plugin for qdev-host: an ABI v2 test device without real function.
 * It exercises every v2 callback and host service so the host can be checked
 * without a VM (tools/vfu-test-client, or a stock QEMU without a guest).
 *
 *   PCI 1234:7e57, class 03.80, INTx + one MSI vector
 *   BAR0 4 KiB MMIO, message-based (no fd: keeps the client's posted writes)
 *
 * What it does:
 *  - dma_register/dma_unregister: log iova, len, fd, fd_offset, vaddr; read the
 *    first word of the region through vaddr and through its own
 *    mmap(fd, fd_offset) and compare; keep a region table for its threads.
 *  - IRQ thread: raises vector 0 at IRQ_HZ from its own thread. Before each IRQ
 *    it writes the 16-byte record {seq, CLOCK_MONOTONIC ns} at guest address
 *    REC, so a client can measure trigger -> eventfd latency.
 *  - render thread: draws frames into a memfd scanout at FPS and announces
 *    them with frame_ready() (QDP_F_PLUGIN_DAMAGE). Pixel (0,0) carries the low
 *    24 bits of the frame seq as R,G,B.
 *  - cursor: 32x32, follows the moving square, new image every 30 frames.
 *  - quiesce/resume: park the IRQ thread (the only one touching guest memory or
 *    raising IRQs) and check the host's rules: dma_unregister only while
 *    quiesced, every callback on the setup thread.
 *
 * BAR0 registers (32-bit):
 *   0x00 ID          RO 0x7e570002
 *   0x04 IRQ_HZ      RW IRQ thread rate, 0 = stopped
 *   0x08 IRQS_OK     RO irq_trigger() calls that returned 0
 *   0x0c IRQS_DROP   RO irq_trigger() calls that returned -errno
 *   0x10 REC_LO, 0x14 REC_HI      RW guest address of the IRQ record (0 = none)
 *   0x18 PROBE_LO, 0x1c PROBE_HI  W  writing PROBE_HI reads the u64 at that guest
 *                                     address through vaddr and through mmap(fd)
 *   0x20 PROBE_VAL_LO, 0x24 PROBE_VAL_HI  RO value read through vaddr (or fd)
 *   0x28 PROBE_STATUS RO 1 equal, 2 different, 3 not mapped, 4 vaddr only, 5 fd only
 *   0x2c FPS         RW render rate, 0 = stopped
 *   0x30 FRAMES      RO frames announced
 *   0x34 VIOLATIONS  RO host rule violations seen (must stay 0)
 *   0x38 QUIESCES    RO
 *   0x3c REGIONS     RO regions currently registered
 *   0x40 IRQ_NS_AVG, 0x44 IRQ_NS_MAX  RO duration of the irq_trigger() call
 *   0x48 COMPARES    RO region words compared equal through vaddr and fd
 *
 * Plugin args: "irq_hz=N,fps=N,mode=WxH,exit_on_disconnect=1" (defaults 0, 0,
 * 640x480, 0). With exit_on_disconnect reset(LOST_CONN) returns QDP_RESET_EXIT.
 * reset(LOST_CONN) also checks that the regions are still mapped (it reads the
 * first word of each through vaddr) and logs how many were.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "qdp.h"
#include "pci_caps/msi.h"

#define MMIO_SIZE  0x1000
#define PIXMAN_x8r8g8b8 0x20020888u
#define CUR_W 32
#define CUR_H 32
#define MAX_REGIONS 64

typedef struct {
    qdp_dma_region r;
    bool used;
} region_t;

typedef struct t2 {
    const qdp_host_ops *host;
    pthread_t setup_thread;

    /* guest memory gate: the IRQ thread touches guest memory and raises IRQs
     * only between gate_enter() and gate_leave() */
    pthread_mutex_t m;
    pthread_cond_t cv;
    bool quiescing, stopping;
    int active;

    pthread_mutex_t rm;               /* region table */
    region_t regions[MAX_REGIONS];

    /* BAR0 state: written on the main loop, read by the threads (and the other
     * way round for the counters), always through LD()/ST()/ADD() */
    uint32_t irq_hz, fps;
    uint64_t rec;
    uint64_t probe, probe_val;
    uint32_t probe_status;
    uint64_t irqs_ok, irqs_drop, irq_ns_sum, irq_ns_max, frames, violations, quiesces, compares;
    bool exit_on_disconnect;
    int lost_conn_regions;            /* regions still registered at reset(LOST_CONN), -1 = none yet */

    /* scanout + cursor */
    uint32_t w, h;
    int fb_fd;
    uint32_t *fb;
    pthread_mutex_t cm;
    int32_t cx, cy;
    uint64_t cserial;
    uint32_t cimg[2][CUR_W * CUR_H];

    pthread_t irq_thr, render_thr;
} t2;

static t2 *g;   /* libvfio-user region callbacks get no private pointer */

#define TLOG(lvl, ...) g->host->log(g->host->host, lvl, __VA_ARGS__)
#define LD(x)     __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define ST(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define ADD(x, v) __atomic_add_fetch(&(x), (v), __ATOMIC_RELAXED)

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static void violation(const char *what)
{
    __atomic_add_fetch(&g->violations, 1, __ATOMIC_RELAXED);
    TLOG(0, "VIOLATION: %s", what);
}
static void check_main_thread(const char *what)
{
    if (!pthread_equal(pthread_self(), g->setup_thread)) violation(what);
}

/* ------------------------------------------------------------- gate */
static bool gate_enter(t2 *d)
{
    pthread_mutex_lock(&d->m);
    while (d->quiescing && !d->stopping) pthread_cond_wait(&d->cv, &d->m);
    bool ok = !d->stopping;
    if (ok) d->active++;
    pthread_mutex_unlock(&d->m);
    return ok;
}
static void gate_leave(t2 *d)
{
    pthread_mutex_lock(&d->m);
    if (--d->active == 0) pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
}

/* ------------------------------------------------------------ regions */
/* host pointer for [iova, iova + len) through the region's vaddr, or NULL */
static void *guest_ptr(t2 *d, uint64_t iova, uint64_t len, region_t *out)
{
    void *p = NULL;
    pthread_mutex_lock(&d->rm);
    for (int i = 0; i < MAX_REGIONS; i++) {
        region_t *r = &d->regions[i];
        if (!r->used || iova < r->r.iova || iova + len > r->r.iova + r->r.len) continue;
        if (out) *out = *r;
        if (r->r.vaddr) p = (uint8_t *)r->r.vaddr + (iova - r->r.iova);
        break;
    }
    pthread_mutex_unlock(&d->rm);
    return p;
}

/* read the u64 at file offset off of fd with a private mapping of the plugin */
static bool fd_read_u64(int fd, uint64_t off, uint64_t *val)
{
    long page = sysconf(_SC_PAGESIZE);
    uint64_t base = off & ~(uint64_t)(page - 1);
    size_t len = (off - base) + sizeof(uint64_t) > (size_t)page ? 2 * page : page;
    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, base);
    if (m == MAP_FAILED) return false;
    memcpy(val, (uint8_t *)m + (off - base), sizeof(*val));
    munmap(m, len);
    return true;
}

/* compare the u64 at iova through vaddr and through mmap(fd); PROBE_STATUS code */
static uint32_t probe(t2 *d, uint64_t iova, uint64_t *val)
{
    region_t r;
    void *p = guest_ptr(d, iova, 8, &r);
    if (!p && !(r.r.len && iova >= r.r.iova && iova + 8 <= r.r.iova + r.r.len)) return 3;
    uint64_t a = 0, b = 0;
    bool have_a = p != NULL, have_b = r.r.fd >= 0 && fd_read_u64(r.r.fd, r.r.fd_offset + (iova - r.r.iova), &b);
    if (have_a) memcpy(&a, p, sizeof(a));
    *val = have_a ? a : b;
    if (have_a && have_b) return a == b ? 1 : 2;
    return have_a ? 4 : have_b ? 5 : 3;
}

static void t2_dma_register(void *priv, const qdp_dma_region *reg)
{
    t2 *d = priv;
    check_main_thread("dma_register off the main loop");
    struct stat st = { 0 };
    if (reg->fd >= 0) fstat(reg->fd, &st);
    pthread_mutex_lock(&d->rm);
    int slot = -1;
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (d->regions[i].used && d->regions[i].r.iova == reg->iova) { pthread_mutex_unlock(&d->rm); violation("dma_register of a region already registered"); return; }
        if (!d->regions[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) { d->regions[slot].r = *reg; d->regions[slot].used = true; }
    pthread_mutex_unlock(&d->rm);
    uint64_t v = 0;
    uint32_t st_code = probe(d, reg->iova, &v);
    static const char *what[] = { "?", "vaddr == mmap(fd)", "vaddr != mmap(fd)", "not readable", "vaddr only", "fd only" };
    if (st_code == 1) d->compares++;
    if (st_code == 2) violation("vaddr and mmap(fd, fd_offset) differ");
    TLOG(2, "dma_register iova 0x%llx len 0x%llx fd %d (ino %llu) fd_offset 0x%llx vaddr %p prot %x%s: word 0x%016llx, %s",
         (unsigned long long)reg->iova, (unsigned long long)reg->len, reg->fd, (unsigned long long)st.st_ino,
         (unsigned long long)reg->fd_offset, reg->vaddr, reg->prot, d->quiescing ? " (quiesced)" : "",
         (unsigned long long)v, what[st_code]);
    if (slot < 0) violation("region table full");
}

static void t2_dma_unregister(void *priv, const qdp_dma_region *reg)
{
    t2 *d = priv;
    check_main_thread("dma_unregister off the main loop");
    pthread_mutex_lock(&d->m);
    bool ok = d->quiescing && d->active == 0;
    pthread_mutex_unlock(&d->m);
    if (!ok) violation("dma_unregister without quiesce");
    bool found = false;
    pthread_mutex_lock(&d->rm);
    for (int i = 0; i < MAX_REGIONS; i++)
        if (d->regions[i].used && d->regions[i].r.iova == reg->iova && d->regions[i].r.len == reg->len) { d->regions[i].used = false; found = true; }
    pthread_mutex_unlock(&d->rm);
    if (!found) violation("dma_unregister of an unknown region");
    /* the fd must still be open here */
    if (reg->fd >= 0 && fcntl(reg->fd, F_GETFD) < 0) violation("region fd closed before dma_unregister returned");
    TLOG(2, "dma_unregister iova 0x%llx len 0x%llx fd %d", (unsigned long long)reg->iova, (unsigned long long)reg->len, reg->fd);
}

static int t2_quiesce(void *priv)
{
    t2 *d = priv;
    check_main_thread("quiesce off the main loop");
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&d->m);
    if (d->quiescing) { pthread_mutex_unlock(&d->m); violation("quiesce while quiesced"); return 0; }
    d->quiescing = true;
    while (d->active > 0) pthread_cond_wait(&d->cv, &d->m);
    pthread_mutex_unlock(&d->m);
    d->quiesces++;
    TLOG(3, "quiesced in %llu us", (unsigned long long)(now_ns() - t0) / 1000);
    return 0;
}

static void t2_resume(void *priv)
{
    t2 *d = priv;
    check_main_thread("resume off the main loop");
    pthread_mutex_lock(&d->m);
    if (!d->quiescing) violation("resume without quiesce");
    d->quiescing = false;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
    TLOG(3, "resumed");
}

/* ------------------------------------------------------------ threads */
static void sleep_ns(uint64_t ns) { struct timespec t = { ns / 1000000000ull, ns % 1000000000ull }; nanosleep(&t, NULL); }

static void *irq_thread(void *arg)
{
    t2 *d = arg;
    uint64_t seq = 0, next = now_ns();
    for (;;) {
        uint32_t hz = LD(d->irq_hz);
        if (!hz) { if (LD(d->stopping)) break; sleep_ns(10000000); next = now_ns(); continue; }
        if (!gate_enter(d)) break;
        uint64_t rec = LD(d->rec);
        uint64_t *p = rec ? guest_ptr(d, rec, 16, NULL) : NULL;
        seq++;
        uint64_t t0 = now_ns();
        if (p) { __atomic_store_n(&p[0], seq, __ATOMIC_RELAXED); __atomic_store_n(&p[1], t0, __ATOMIC_RELEASE); }
        int r = d->host->irq_trigger(d->host->host, 0);
        uint64_t dt = now_ns() - t0;
        gate_leave(d);
        if (r == 0) {
            ADD(d->irqs_ok, 1); ADD(d->irq_ns_sum, dt);
            if (dt > LD(d->irq_ns_max)) ST(d->irq_ns_max, dt);
        } else {
            ADD(d->irqs_drop, 1);
        }
        next += 1000000000ull / hz;
        uint64_t now = now_ns();
        if (next > now) sleep_ns(next - now); else next = now;
    }
    return NULL;
}

static void draw_cursor_images(t2 *d)
{
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            bool arrow = x <= y && x < 20 && y < 28;               /* image 0: white triangle, black edge */
            bool edge = arrow && (x == 0 || x == y || y == 27 || x == 19);
            d->cimg[0][y * CUR_W + x] = arrow ? (edge ? 0xff000000u : 0xffffffffu) : 0;
            bool ring = abs(x - 16) + abs(y - 16) <= 14 && abs(x - 16) + abs(y - 16) >= 10;
            d->cimg[1][y * CUR_W + x] = ring ? 0xffff2020u : 0;    /* image 1: red diamond ring */
        }
}

static void *render_thread(void *arg)
{
    t2 *d = arg;
    uint64_t seq = 0, next = now_ns();
    int bx = 0, by = 0, dx = 7, dy = 5;
    const int B = 64;
    for (;;) {
        uint32_t fps = LD(d->fps);
        if (LD(d->stopping)) break;
        if (!fps) { sleep_ns(10000000); next = now_ns(); continue; }
        seq++;
        int ox = bx, oy = by;
        bx += dx; by += dy;
        if (bx < 0 || bx + B > (int)d->w) { dx = -dx; bx += 2 * dx; }
        if (by < 0 || by + B > (int)d->h) { dy = -dy; by += 2 * dy; }
        bool full = seq % 30 == 1;
        uint32_t bg = 0x00203040u + ((uint32_t)(seq / 30 % 8) << 4);
        if (full) for (size_t i = 0; i < (size_t)d->w * d->h; i++) d->fb[i] = bg;
        else for (int y = oy; y < oy + B; y++) for (int x = ox; x < ox + B; x++) d->fb[(size_t)y * d->w + x] = bg;
        for (int y = by; y < by + B; y++) for (int x = bx; x < bx + B; x++) d->fb[(size_t)y * d->w + x] = 0x00e0c020u;
        d->fb[0] = (uint32_t)(seq & 0xffffff);                     /* seq in pixel (0,0) */
        /* damage: old and new square plus pixel (0,0), i.e. [0, max right) x [0, max bottom) */
        qdp_frame_info f = { .seq = seq };
        if (!full) { f.w = (ox > bx ? ox : bx) + B; f.h = (oy > by ? oy : by) + B; }
        ST(d->frames, seq);
        d->host->frame_ready(d->host->host, &f);
        pthread_mutex_lock(&d->cm);
        d->cx = bx + B / 2; d->cy = by + B / 2;
        if (seq % 30 == 1) d->cserial++;
        pthread_mutex_unlock(&d->cm);
        d->host->cursor_changed(d->host->host);
        next += 1000000000ull / fps;
        uint64_t now = now_ns();
        if (next > now) sleep_ns(next - now); else next = now;
    }
    return NULL;
}

/* ------------------------------------------------------------ BAR0 */
static ssize_t bar0_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{
    t2 *d = g;
    uint32_t v = 0;
    check_main_thread("BAR0 access off the main loop");
    if (n != 4 || (off & 3)) { if (!w) memset(buf, 0, n); return n; }
    if (w) {
        memcpy(&v, buf, 4);
        switch (off) {
        case 0x04: ST(d->irq_hz, v); TLOG(2, "IRQ thread at %u Hz", v); break;
        case 0x10: ST(d->rec, (LD(d->rec) & ~0xffffffffull) | v); break;
        case 0x14: ST(d->rec, (LD(d->rec) & 0xffffffffull) | ((uint64_t)v << 32)); break;
        case 0x18: d->probe = (d->probe & ~0xffffffffull) | v; break;
        case 0x1c:
            d->probe = (d->probe & 0xffffffffull) | ((uint64_t)v << 32);
            d->probe_status = probe(d, d->probe, &d->probe_val);
            if (d->probe_status == 1) d->compares++;
            if (d->probe_status == 2) violation("probe: vaddr and mmap(fd, fd_offset) differ");
            TLOG(2, "probe 0x%llx: 0x%016llx status %u", (unsigned long long)d->probe, (unsigned long long)d->probe_val, d->probe_status);
            break;
        case 0x2c: ST(d->fps, v); TLOG(2, "rendering at %u fps", v); break;
        }
        return n;
    }
    switch (off) {
    case 0x00: v = 0x7e570002; break;
    case 0x04: v = LD(d->irq_hz); break;
    case 0x08: v = LD(d->irqs_ok); break;
    case 0x0c: v = LD(d->irqs_drop); break;
    case 0x10: v = LD(d->rec); break;
    case 0x14: v = LD(d->rec) >> 32; break;
    case 0x18: v = d->probe; break;
    case 0x1c: v = d->probe >> 32; break;
    case 0x20: v = d->probe_val; break;
    case 0x24: v = d->probe_val >> 32; break;
    case 0x28: v = d->probe_status; break;
    case 0x2c: v = LD(d->fps); break;
    case 0x30: v = LD(d->frames); break;
    case 0x34: v = __atomic_load_n(&d->violations, __ATOMIC_RELAXED); break;
    case 0x38: v = d->quiesces; break;
    case 0x3c: { pthread_mutex_lock(&d->rm); for (int i = 0; i < MAX_REGIONS; i++) v += d->regions[i].used; pthread_mutex_unlock(&d->rm); break; }
    case 0x40: { uint64_t n = LD(d->irqs_ok); v = n ? LD(d->irq_ns_sum) / n : 0; break; }
    case 0x44: v = LD(d->irq_ns_max); break;
    case 0x48: v = d->compares; break;
    }
    memcpy(buf, &v, 4);
    return n;
}

/* ------------------------------------------------------------ ops */
static bool t2_get_scanout(void *priv, qdp_scanout *out)
{
    t2 *d = priv;
    check_main_thread("get_scanout off the main loop");
    *out = (qdp_scanout){ .enabled = true, .fd = d->fb_fd, .width = d->w, .height = d->h, .stride = d->w * 4,
                          .pixman_format = PIXMAN_x8r8g8b8 };
    return true;
}

static bool t2_get_cursor(void *priv, qdp_cursor *out)
{
    t2 *d = priv;
    check_main_thread("get_cursor off the main loop");
    pthread_mutex_lock(&d->cm);
    *out = (qdp_cursor){ .visible = true, .x = d->cx - 1, .y = d->cy - 1, .hot_x = 1, .hot_y = 1,
                         .width = CUR_W, .height = CUR_H, .pixels = d->cimg[d->cserial & 1], .serial = d->cserial };
    pthread_mutex_unlock(&d->cm);
    return true;
}

static int t2_reset(void *priv, vfu_reset_type_t type)
{
    t2 *d = priv;
    check_main_thread("reset off the main loop");
    pthread_mutex_lock(&d->m);
    bool q = d->quiescing;
    pthread_mutex_unlock(&d->m);
    TLOG(2, "reset type %d%s", type, q ? " (quiesced)" : "");
    if (!q) violation("reset without quiesce");
    ST(d->irq_hz, 0); ST(d->rec, 0);
    if (type != VFU_RESET_LOST_CONN) return 0;
    /* the regions must still be mapped here: read the first word of each */
    int n = 0;
    uint64_t sum = 0;
    pthread_mutex_lock(&d->rm);
    for (int i = 0; i < MAX_REGIONS; i++) {
        if (!d->regions[i].used) continue;
        n++;
        if (d->regions[i].r.vaddr) sum += *(volatile uint64_t *)d->regions[i].r.vaddr;
    }
    pthread_mutex_unlock(&d->rm);
    d->lost_conn_regions = n;
    TLOG(2, "lost connection with %d regions still mapped (checksum 0x%llx)%s", n, (unsigned long long)sum,
         d->exit_on_disconnect ? ": cannot serve another client, QDP_RESET_EXIT" : "");
    return d->exit_on_disconnect ? QDP_RESET_EXIT : 0;
}

static int t2_setup(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env, void **priv)
{
    if (env->size < offsetof(qdp_host_env, host_ops) + sizeof(env->host_ops) || !env->host_ops ||
        env->host_ops->size < sizeof(qdp_host_ops))
        return -EINVAL;
    t2 *d = calloc(1, sizeof(*d));
    d->host = env->host_ops;
    d->setup_thread = pthread_self();
    d->w = 640; d->h = 480; d->lost_conn_regions = -1;
    unsigned irq_hz = 0, fps = 0;
    if (args) {
        char *copy = strdup(args), *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (!strncmp(tok, "irq_hz=", 7)) irq_hz = strtoul(tok + 7, NULL, 10);
            else if (!strncmp(tok, "fps=", 4)) fps = strtoul(tok + 4, NULL, 10);
            else if (!strncmp(tok, "mode=", 5)) sscanf(tok + 5, "%ux%u", &d->w, &d->h);
            else if (!strncmp(tok, "exit_on_disconnect=", 19)) d->exit_on_disconnect = atoi(tok + 19) != 0;
            else { d->host->log(d->host->host, 0, "unknown arg '%s'", tok); free(copy); free(d); return -EINVAL; }
        }
        free(copy);
    }
    if (d->w < 128 || d->h < 128 || d->w > 8192 || d->h > 8192) { free(d); return -EINVAL; }
    pthread_mutex_init(&d->m, NULL); pthread_cond_init(&d->cv, NULL);
    pthread_mutex_init(&d->rm, NULL); pthread_mutex_init(&d->cm, NULL);
    size_t fb_len = (size_t)d->w * d->h * 4;
    d->fb_fd = memfd_create("test-v2-scanout", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (d->fb_fd < 0 || ftruncate(d->fb_fd, fb_len) < 0) { free(d); return -errno; }
    fcntl(d->fb_fd, F_ADD_SEALS, F_SEAL_SHRINK);
    d->fb = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fb_fd, 0);
    if (d->fb == MAP_FAILED) { close(d->fb_fd); free(d); return -errno; }
    draw_cursor_images(d);
    g = d;

    if (vfu_pci_init(ctx, VFU_PCI_TYPE_CONVENTIONAL, PCI_HEADER_TYPE_NORMAL, 0) < 0) return -errno;
    vfu_pci_set_id(ctx, 0x1234, 0x7e57, 0x1af4, 0x1100);
    vfu_pci_set_class(ctx, 0x03, 0x80, 0x00);
    struct msicap msi = { .hdr = { .id = PCI_CAP_ID_MSI }, .mc = { .c64 = 1 } };   /* one vector, 64-bit, no mask */
    if (vfu_pci_add_capability(ctx, 0, 0, &msi) < 0) return -errno;
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, MMIO_SIZE, bar0_cb, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
                         NULL, 0, -1, 0) < 0) return -errno;
    if (vfu_setup_device_nr_irqs(ctx, VFU_DEV_INTX_IRQ, 1) < 0 || vfu_setup_device_nr_irqs(ctx, VFU_DEV_MSI_IRQ, 1) < 0) return -errno;
    /* v2: no vfu_setup_device_dma()/vfu_setup_device_quiesce_cb() here, the host owns them */
    ST(d->irq_hz, irq_hz); ST(d->fps, fps);
    if (pthread_create(&d->irq_thr, NULL, irq_thread, d) || pthread_create(&d->render_thr, NULL, render_thread, d)) return -EAGAIN;
    *priv = d;
    TLOG(2, "test-v2 %ux%u, irq_hz %u, fps %u", d->w, d->h, irq_hz, fps);
    return 0;
}

static void t2_teardown(void *priv)
{
    t2 *d = priv;
    if (!d) return;
    pthread_mutex_lock(&d->m);
    ST(d->stopping, true);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->m);
    pthread_join(d->irq_thr, NULL); pthread_join(d->render_thr, NULL);
    TLOG(2, "teardown: irqs ok %llu dropped %llu (trigger avg %llu ns max %llu ns), frames %llu, quiesces %llu, compares %llu, "
         "regions at lost connection %d, violations %llu",
         (unsigned long long)d->irqs_ok, (unsigned long long)d->irqs_drop,
         (unsigned long long)(d->irqs_ok ? d->irq_ns_sum / d->irqs_ok : 0), (unsigned long long)d->irq_ns_max,
         (unsigned long long)d->frames, (unsigned long long)d->quiesces, (unsigned long long)d->compares,
         d->lost_conn_regions, (unsigned long long)d->violations);
    munmap(d->fb, (size_t)d->w * d->h * 4); close(d->fb_fd);
    if (g == d) g = NULL;
    free(d);
}

static const qdp_device_ops ops = {
    .abi_version = QDP_ABI_VERSION,
    .name = "test-v2",
    .version = "0.1",
    .setup = t2_setup,
    .teardown = t2_teardown,
    .reset = t2_reset,
    .get_scanout = t2_get_scanout,
    .flags = QDP_F_PLUGIN_DAMAGE,
    .quiesce = t2_quiesce,
    .resume = t2_resume,
    .dma_register = t2_dma_register,
    .dma_unregister = t2_dma_unregister,
    .get_cursor = t2_get_cursor,
};

const qdp_device_ops *qdp_plugin_v1(void) { return &ops; }
