/*
 * pvg_process_vm_test.c - off-VM native unit test of the uniq-vgpu process-VM provider
 * (eeee_task_* callbacks) and of the ordinary physical capture V2, run against the
 * production pvg-process-vm.c / pvg-memory.c / pvg-dmabuf.c. No VM, no GPU, no libmlapi.
 *
 * Contract PVM-RANGE-NOHANDLE-01 (D-QDEV-MEMFD-FD-GROWTH, 2026-11-21):
 *   Reference: ParavirtualizedGraphicsGPUTask (x86_64, pvg-reference-truth 20260622)
 *   PGMemoryMap -mapMemoryAtVirtualAddress:ranges:rangeCount:readOnly: (0x1000084e1) and
 *   -newRangeMapping:rangeCount:totalLength:readOnly: (0x1000081ca) map every physical
 *   range with FUN_100008482 = mach_vm_remap(task_self, FIXED|OVERWRITE 0x4000, copy=0,
 *   inherit=1) from the guest-RAM region and keep only the VA/length/writable record
 *   (PGMapMapping). A mapped range owns its VM entry (which references the memory object)
 *   and no per-range handle. Plugin side: one descriptor per DMA region (README "maps each
 *   DMA region itself from a dup of its fd"); the region reference held by each range
 *   pins that region and its descriptor until the range is gone.
 *   Expected (oracle independent of the implementation): after mapping K single-page ranges
 *   (task remap aliases or capture pieces) of one region, the process holds exactly one
 *   descriptor of that region's file, for every K; a complete capture adds exactly its one
 *   udmabuf export; data written through a range is the region's data; the ranges stay
 *   valid after the client unregisters the region (pin), and the region descriptor is
 *   closed exactly when the last range is released.
 *   Preconditions: memfd with F_SEAL_SHRINK|F_SEAL_GROW (QEMU memory-backend-memfd seal=on),
 *   no F_SEAL_WRITE; page-aligned ranges; /dev/udmabuf openable (else the capture-export
 *   cases are reported SKIP, never PASS).
 *
 * Boundary double: pvg_log/pvg_log_direct (the plugin's log sink in uniq-vgpu.c) print to
 * stderr. Everything else is production code.
 *
 * Build + run: make QMETAL_INCLUDE=<qmetal>/include uniq-vgpu-pvm-test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include "pvg-process-vm.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>

#define RAM_NAME "pvm-test-ram"
#define RAM_PAGES 1024u
#define RAM_IOVA 0x100000ull

static int failures, passes, skips;

#define CHECK(c) do { if (c) { passes++; } else { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* boundary double: the plugin's log sink */
void pvg_log(PvgState *s, const char *fmt, ...)
{
    va_list ap;

    (void)s;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void pvg_log_direct(PvgState *s, const char *text)
{
    (void)s;
    fputs(text, stderr);
}

/* descriptors of this process whose target contains `needle` (NULL: all) */
static int count_fds(const char *needle)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0, self = d ? dirfd(d) : -1;

    if (!d) {
        abort();
    }
    while ((e = readdir(d))) {
        char path[300], target[256];
        ssize_t len;

        if (e->d_name[0] == '.' || atoi(e->d_name) == self) {
            continue;
        }
        if (!needle) {
            n++;
            continue;
        }
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        len = readlink(path, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = 0;
            n += strstr(target, needle) != NULL;
        }
    }
    closedir(d);
    return n;
}

static bool udmabuf_available(void)
{
    int fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

static const qdp_dma_region *ram_region_desc(int fd)
{
    static qdp_dma_region desc;

    desc = (qdp_dma_region) {
        .iova = RAM_IOVA, .len = (uint64_t)RAM_PAGES * pvg_page_size(), .vaddr = NULL,
        .fd = fd, .fd_offset = 0, .prot = PROT_READ | PROT_WRITE,
    };
    return &desc;
}

/* The client side: a sealed guest-RAM memfd registered as a DMA region, then the
 * client's own descriptor closed (the plugin keeps its region dup). */
static void register_ram(PvgState *s)
{
    int fd = memfd_create(RAM_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (fd < 0 || ftruncate(fd, (off_t)RAM_PAGES * (off_t)pvg_page_size()) != 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        perror("memfd fixture");
        abort();
    }
    pvg_memory_dma_register(s, ram_region_desc(fd));
    close(fd);
}

static PvgRegion *ram_region(PvgState *s)
{
    PvgRegion *r;

    pvg_memory_read_enter(s);
    r = pvg_memory_lookup_locked(s, RAM_IOVA);
    pvg_memory_read_leave(s);
    return r;
}

static unsigned region_refs(PvgState *s, PvgRegion *r)
{
    unsigned refs;

    pthread_mutex_lock(&s->mem.live_lock);
    refs = r->refs;
    pthread_mutex_unlock(&s->mem.live_lock);
    return refs;
}

static unsigned live_regions(PvgState *s)
{
    unsigned n;

    pthread_mutex_lock(&s->mem.live_lock);
    n = s->mem.live->len;
    pthread_mutex_unlock(&s->mem.live_lock);
    return n;
}

/* ---------------------------------------------------------------- remap aliases */
static void test_remap_aliases(unsigned k)
{
    PvgState *s = g_new0(PvgState, 1);
    AgfxEeeeNativeContext *context;
    qmu_extended_callbacks cb = { 0 };
    const uint64_t page = pvg_page_size();
    uint64_t task = 0;
    void *region = NULL;
    uint64_t region_offset = 0, covered = 0;
    PvgRegion *r;
    uint8_t *ram;
    const int fds0 = count_fds(NULL);

    fprintf(stderr, "== remap aliases k=%u\n", k);
    pvg_memory_init(&s->mem);
    register_ram(s);
    r = ram_region(s);
    CHECK(r && r->map && r->fd >= 0);
    CHECK(count_fds(RAM_NAME) == 1);
    ram = r->map;
    context = agfx_eeee_native_context_create(s);
    CHECK(context && agfx_eeee_native_context_acquire(context, &cb));
    CHECK(cb.eeee_task_allocate(cb.user_ctx, &task, 2ull * k * page,
                                QMU_EEEE_TASK_ALLOCATE_ANYWHERE) == 1);

    /* LegacyTaskProvider::map: under rcu + task lock, region_ref once, then one remap
     * per physical range; every other guest page, so no two ranges are contiguous. */
    cb.eeee_task_rcu_enter(cb.user_ctx);
    cb.eeee_task_lock(cb.user_ctx);
    CHECK(cb.eeee_task_translate(cb.user_ctx, RAM_IOVA, page, 1, &region,
                                 &region_offset, &covered) == 1);
    CHECK(region == r && region_offset == 0 && covered == page);
    cb.eeee_task_region_ref(cb.user_ctx, region);
    for (unsigned i = 0; i < k; i++) {
        uint64_t target = task + (uint64_t)i * page;
        int cur = 0, max = 0;
        uint64_t source = (uint64_t)(uintptr_t)(ram + (uint64_t)(2 * i) * page);

        CHECK(cb.eeee_task_remap_shared_fixed_overwrite(cb.user_ctx, &target, page,
                                                        page - 1, source, &cur, &max) == 1);
    }
    cb.eeee_task_unlock(cb.user_ctx);
    cb.eeee_task_rcu_leave(cb.user_ctx);

    /* PVM-RANGE-NOHANDLE-01: the region's one descriptor, independent of k */
    fprintf(stderr, "   region-file descriptors after %u remaps: %d (expected 1)\n", k,
            count_fds(RAM_NAME));
    CHECK(count_fds(RAM_NAME) == 1);
    CHECK(count_fds(NULL) == fds0 + 1);
    /* each alias holds its own region pin: table + task region_ref + k aliases */
    CHECK(region_refs(s, r) == 2 + k);

    /* data identity both ways */
    for (unsigned i = 0; i < k; i++) {
        uint8_t *alias = (uint8_t *)(uintptr_t)(task + (uint64_t)i * page);

        ram[(uint64_t)(2 * i) * page + 7] = (uint8_t)(0x40 + i);
        CHECK(alias[7] == (uint8_t)(0x40 + i));
        alias[9] = (uint8_t)(0x80 + i);
        CHECK(ram[(uint64_t)(2 * i) * page + 9] == (uint8_t)(0x80 + i));
    }

    /* LegacyTaskProvider::unmap of the first range: fixed overwrite retires its alias */
    {
        uint64_t first = task;

        CHECK(cb.eeee_task_allocate(cb.user_ctx, &first, page,
                                    QMU_EEEE_TASK_ALLOCATE_FIXED_OVERWRITE) == 1);
        CHECK(((uint8_t *)(uintptr_t)task)[7] == 0);
        CHECK(region_refs(s, r) == 2 + k - 1);
        CHECK(count_fds(RAM_NAME) == 1);
    }

    /* the client unregisters the region while aliases are live: they stay valid (pin) */
    pvg_memory_dma_unregister(s, ram_region_desc(-1));
    CHECK(ram_region(s) == NULL);
    CHECK(live_regions(s) == 1);
    if (k > 1) {
        uint8_t *alias = (uint8_t *)(uintptr_t)(task + page);

        CHECK(alias[7] == (uint8_t)(0x41));
    }
    CHECK(count_fds(RAM_NAME) == 1);

    /* LegacyTaskDeleter: region_unref, then deallocate: the last pin closes the file */
    cb.eeee_task_region_unref(cb.user_ctx, region);
    CHECK(count_fds(RAM_NAME) == (k > 1 ? 1 : 0));
    CHECK(cb.eeee_task_deallocate(cb.user_ctx, task, 2ull * k * page) == 0);
    CHECK(live_regions(s) == 0);
    CHECK(count_fds(RAM_NAME) == 0);
    CHECK(count_fds(NULL) == fds0);

    agfx_eeee_native_context_release(context);
    CHECK(agfx_eeee_native_context_retire(context));
    g_ptr_array_free(s->mem.live, true);
    g_free(s->mem.tab);
    g_free(s);
}

/* ---------------------------------------------------------------- capture pieces */
static void *capture(struct qmu_ordinary_physical_handoff_v2 *h, PvgState *s,
                     unsigned k, unsigned first_page)
{
    const uint64_t page = pvg_page_size();
    struct qmu_ordinary_physical_piece_v2 *pieces = g_new0(struct qmu_ordinary_physical_piece_v2, k);
    struct qmu_ordinary_physical_request_v2 request = {
        .version = QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_VERSION,
        .struct_size = sizeof(request),
        .task_id = 7, .resource_id = 9,
        .allocation_length = (uint64_t)k * page,
        .visible_offset = 0,
        .literal_length = (uint64_t)k * page,
        .allocation_alignment = page,
        .piece_count = k,
        .pieces = pieces,
    };
    PvgRegion *r = ram_region(s);
    void *span = NULL;

    for (unsigned i = 0; i < k; i++) {
        uint64_t offset = (uint64_t)(first_page + 2 * i) * page;

        pieces[i] = (struct qmu_ordinary_physical_piece_v2) {
            .gpa = RAM_IOVA + offset, .logical_offset = (uint64_t)i * page,
            .length = page, .mapped_pointer = r->map + offset,
        };
    }
    h->capture(h->acquired_context, &request, &span);
    g_free(pieces);
    return span;
}

static void test_capture_pieces(unsigned k)
{
    PvgState *s = g_new0(PvgState, 1);
    AgfxOrdinaryPhysicalContext *context;
    struct qmu_ordinary_physical_handoff_v2 h = { 0 };
    struct qmu_ordinary_physical_view_v2 view = { 0 };
    const uint64_t page = pvg_page_size();
    const bool udmabuf = udmabuf_available();
    void *span, *other;
    PvgRegion *r;
    bool described;
    const int fds0 = count_fds(NULL);

    fprintf(stderr, "== capture pieces k=%u (udmabuf %s)\n", k, udmabuf ? "available" : "UNAVAILABLE");
    pvg_memory_init(&s->mem);
    register_ram(s);
    r = ram_region(s);
    CHECK(r && r->map);
    context = agfx_ordinary_physical_context_create(s);
    CHECK(context && agfx_ordinary_physical_context_acquire(context, &h));

    span = capture(&h, s, k, 0);       /* even pages */
    CHECK(span != NULL);
    described = h.describe(span, &view);
    fprintf(stderr, "   region-file descriptors after a %u-piece capture: %d (expected 1)\n", k,
            count_fds(RAM_NAME));
    /* PVM-RANGE-NOHANDLE-01: the region's one descriptor + the one export */
    CHECK(count_fds(RAM_NAME) == 1);
    CHECK(region_refs(s, r) == 1 + k);
    if (!udmabuf) {
        skips++;
        fprintf(stderr, "SKIP capture export/content cases: /dev/udmabuf not openable\n");
        CHECK(!described);
    } else {
        CHECK(described);
        CHECK(count_fds(NULL) == fds0 + 2);              /* region fd + udmabuf export */
        CHECK(count_fds("dmabuf") >= 1);
        CHECK(view.allocation_length == (uint64_t)k * page && view.dma_buf_fd >= 0);
        /* data identity: span page i is guest page 2i */
        for (unsigned i = 0; i < k; i++) {
            uint8_t *v = (uint8_t *)view.visible_pointer + (uint64_t)i * page;

            r->map[(uint64_t)(2 * i) * page + 3] = (uint8_t)(0x20 + i);
            CHECK(v[3] == (uint8_t)(0x20 + i));
        }
        /* comparison identity (fstat of each piece's file): same span overlaps itself,
         * a span of the odd pages is disjoint */
        other = capture(&h, s, k, 1);
        CHECK(other != NULL);
        CHECK(count_fds(RAM_NAME) == 1);
        CHECK(agfx_ordinary_physical_compare(context, span, 0, (uint64_t)k * page,
                                             span, 0, (uint64_t)k * page) ==
              QMU_ORDINARY_PHYSICAL_COMPARISON_OVERLAP);
        CHECK(agfx_ordinary_physical_compare(context, span, 0, (uint64_t)k * page,
                                             other, 0, (uint64_t)k * page) ==
              QMU_ORDINARY_PHYSICAL_COMPARISON_DISJOINT);
        h.release_span(other);
        CHECK(region_refs(s, r) == 1 + k);
    }

    /* the client unregisters the region while the span is live: still valid (pin) */
    pvg_memory_dma_unregister(s, ram_region_desc(-1));
    CHECK(ram_region(s) == NULL);
    CHECK(live_regions(s) == 1);
    CHECK(count_fds(RAM_NAME) == 1);
    if (udmabuf) {
        CHECK(((uint8_t *)view.visible_pointer)[3] == 0x20);
        CHECK(agfx_ordinary_physical_compare(context, span, 0, page, span, 0, page) ==
              QMU_ORDINARY_PHYSICAL_COMPARISON_OVERLAP);
    }

    /* release: export, then the pieces; the last pin closes the region file */
    h.release_span(span);
    CHECK(live_regions(s) == 0);
    CHECK(count_fds(RAM_NAME) == 0);
    CHECK(count_fds(NULL) == fds0);

    h.release_context(h.acquired_context);
    agfx_ordinary_physical_context_retire(context);
    g_ptr_array_free(s->mem.live, true);
    g_free(s->mem.tab);
    g_free(s);
}

/* ------------------------------------------------- guest-RAM region bounds (ML-SGR-2) */
/* qmu_guest_ram_region_v1.h: the bounds of the region holding a GPA, taken without a pin and
 * without a log.  QMetal maps a 4 MiB chunk only when it lies inside the answer; the 4 GiB A/B
 * (2026-11-23) logged "map_gpa ... crosses the end of its DMA region" from a chunk probe. */
static void test_guest_ram_region(void)
{
    PvgState *s = g_new0(PvgState, 1);
    const uint64_t page = pvg_page_size(), len = (uint64_t)RAM_PAGES * page, chunk = 4ull << 20;
    uint64_t base = 7, length = 9, chunk_start;
    PvgRegion *r;
    unsigned refs;

    fprintf(stderr, "== guest-RAM region bounds\n");
    pvg_memory_init(&s->mem);
    CHECK(pvg_guest_ram_region(s, RAM_IOVA, &base, &length) == 0);  /* nothing registered */
    register_ram(s);
    r = ram_region(s);
    CHECK(r && r->map);
    refs = region_refs(s, r);
    CHECK(pvg_guest_ram_region(s, RAM_IOVA + page, &base, &length) == 1);
    CHECK(base == RAM_IOVA && length == len);
    CHECK(region_refs(s, r) == refs);                                /* no pin */
    base = 7; length = 9;
    CHECK(pvg_guest_ram_region(s, RAM_IOVA - page, &base, &length) == 0);
    CHECK(pvg_guest_ram_region(s, RAM_IOVA + len, &base, &length) == 0);
    CHECK(base == 7 && length == 9);                                 /* untouched on a miss */
    CHECK(pvg_guest_ram_region(NULL, RAM_IOVA, &base, &length) == 0);
    CHECK(pvg_guest_ram_region(s, RAM_IOVA, NULL, &length) == 0);

    /* The chunk holding the last RAM page crosses the region end: the bounds say so and the
     * query logged nothing; the old probe (a map of the whole chunk) is refused AND logged. */
    chunk_start = (RAM_IOVA + len - page) / chunk * chunk;
    CHECK(pvg_guest_ram_region(s, chunk_start < RAM_IOVA ? RAM_IOVA : chunk_start, &base, &length) == 1);
    CHECK(!(base <= chunk_start && chunk_start + chunk <= base + length));
    CHECK(pvg_atomic_read(&s->mem.refused) == 0);
    CHECK(pvg_map_gpa(s, chunk_start, chunk, 0) == NULL);
    CHECK(pvg_atomic_read(&s->mem.refused) == 1);
    CHECK(region_refs(s, r) == refs);

    pvg_memory_dma_unregister(s, ram_region_desc(-1));
    CHECK(pvg_guest_ram_region(s, RAM_IOVA + page, &base, &length) == 0);
    CHECK(live_regions(s) == 0);
    g_ptr_array_free(s->mem.live, true);
    g_free(s->mem.tab);
    g_free(s);
}

int main(void)
{
    const unsigned ks[] = { 1, 16, 256 };

    for (unsigned i = 0; i < G_N_ELEMENTS(ks); i++) {
        test_remap_aliases(ks[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(ks); i++) {
        if (ks[i] * 2 <= RAM_PAGES) {
            test_capture_pieces(ks[i]);
        }
    }
    test_guest_ram_region();
    fprintf(stderr, "pvg_process_vm_test: %d checks passed, %d failed, %d skipped groups\n",
            passes, failures, skips);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
