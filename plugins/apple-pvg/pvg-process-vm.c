/*
 * apple-pvg plugin: EEEE process-VM provider (eeee_task_* callbacks), the
 * native CPU facilities context, and the ordinary physical capture V2.
 *
 * Port of hw/display/apple-gfx-eeee-process-vm.c and the context code of
 * hw/display/apple-gfx-ml.c (QEMU fork c43589029b). The QEMU MemoryRegion
 * identity becomes the plugin's PvgRegion (one client DMA region with its own
 * fd dup and mapping); memory_region_ref/unref become region refs; the RCU
 * read section becomes the region table's read section; qemu_ram_get_fd /
 * _fd_offset become the region's fd and file offset. There is no dirty log
 * (no migration), so dirty marking only validates its range. Not ported: the
 * native EEEE factory's source-backing/dma-buf capability tables, which the
 * production realize never installs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pvg-process-vm.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Identity captured at admitted remap; the dup'ed fd survives the client's
 * unmap, the region reference pins the mapping and the table identity. */
typedef struct AgfxEeeePhysicalBacking {
    int references;
    PvgState *state;
    PvgRegion *region;
    int fd;
    uint64_t file_offset;
} AgfxEeeePhysicalBacking;

typedef struct AgfxEeeeAlias {
    AgfxEeeePhysicalBacking *backing;
    uint64_t target;
    uint64_t region_offset;
    uint64_t length;
} AgfxEeeeAlias;

typedef struct AgfxEeeeSourcePiece {
    AgfxEeeePhysicalBacking *backing;
    uint64_t region_offset;
    uint64_t mapping_offset;
    uint64_t length;
} AgfxEeeeSourcePiece;

struct AgfxEeeeSourceBacking {
    int references;
    void *mapping;
    uint64_t mapped_length;
    uint64_t view_offset;
    uint64_t length;
    GArray *pieces;
};

typedef struct AgfxEeeeAllocation {
    uint64_t address;
    uint64_t length;           /* literal source length */
    uint64_t mapped_length;    /* native page rounding */
    GPtrArray *aliases;
} AgfxEeeeAllocation;

struct AgfxEeeeProviderState {
    pthread_mutex_t lock;
    GHashTable *allocations;     /* &allocation->address -> allocation */
    GHashTable *regions;         /* PvgRegion -> reference count */
    GHashTable *published_tasks; /* QMetal LegacyTaskProvider -> itself */
    GPtrArray *retired_regions;  /* aliases released outside the task lock */
    bool destroying;
};

static bool agfx_eeee_range(uint64_t address, uint64_t length)
{
    return length != 0 && address <= UINT64_MAX - length;
}

static bool agfx_eeee_page_range(uint64_t address, uint64_t length)
{
    const uint64_t page_size = pvg_page_size();

    return page_size != 0 && agfx_eeee_range(address, length) &&
           (address % page_size) == 0 && (length % page_size) == 0;
}

static bool agfx_eeee_contains(uint64_t outer_address, uint64_t outer_length,
                               uint64_t inner_address, uint64_t inner_length)
{
    return agfx_eeee_range(outer_address, outer_length) &&
           agfx_eeee_range(inner_address, inner_length) &&
           inner_address >= outer_address &&
           inner_address - outer_address <= outer_length &&
           inner_length <= outer_length - (inner_address - outer_address);
}

static bool agfx_eeee_round_length(uint64_t length, uint64_t page_size,
                                   uint64_t *rounded)
{
    const uint64_t mask = page_size - 1;

    if (!length || !page_size || (page_size & mask) ||
        length > UINT64_MAX - mask) {
        return false;
    }
    *rounded = (length + mask) & ~mask;
    return *rounded <= SIZE_MAX;
}

static bool agfx_eeee_intersects(uint64_t left_address, uint64_t left_length,
                                 uint64_t right_address, uint64_t right_length)
{
    return agfx_eeee_range(left_address, left_length) &&
           agfx_eeee_range(right_address, right_length) &&
           left_address < right_address + right_length &&
           right_address < left_address + left_length;
}

static void agfx_eeee_physical_backing_ref(AgfxEeeePhysicalBacking *backing)
{
    g_assert_cmpint(pvg_atomic_fetch_inc(&backing->references), >, 0);
    /* Each alias/piece keeps its own region pin, including a split suffix. */
    pvg_region_ref(backing->state, backing->region);
}

static void agfx_eeee_physical_backing_unref(AgfxEeeePhysicalBacking *backing)
{
    PvgRegion *region;
    PvgState *state;

    if (!backing) {
        return;
    }
    region = backing->region;
    state = backing->state;
    if (pvg_atomic_fetch_dec(&backing->references) == 1) {
        if (backing->fd >= 0) {
            close(backing->fd);
        }
        g_free(backing);
    }
    if (region) {
        pvg_region_unref(state, region);
    }
}

static void agfx_eeee_alias_destroy(gpointer data)
{
    AgfxEeeeAlias *alias = data;

    agfx_eeee_physical_backing_unref(alias->backing);
    g_free(alias);
}

static GPtrArray *agfx_eeee_take_retired_regions(AgfxEeeeProviderState *state)
{
    GPtrArray *retired;

    if (state->retired_regions->len == 0) {
        return NULL;
    }
    retired = state->retired_regions;
    state->retired_regions = g_ptr_array_new_with_free_func(agfx_eeee_alias_destroy);
    return retired;
}

static void agfx_eeee_allocation_destroy(gpointer data)
{
    AgfxEeeeAllocation *allocation = data;

    if (!allocation) {
        return;
    }
    g_ptr_array_free(allocation->aliases, true);
    g_free(allocation);
}

AgfxEeeeProviderState *agfx_eeee_provider_state_create(void)
{
    AgfxEeeeProviderState *state = g_new0(AgfxEeeeProviderState, 1);

    pthread_mutex_init(&state->lock, NULL);
    state->allocations = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               NULL, agfx_eeee_allocation_destroy);
    state->regions = g_hash_table_new(g_direct_hash, g_direct_equal);
    state->published_tasks = g_hash_table_new(g_direct_hash, g_direct_equal);
    state->retired_regions = g_ptr_array_new_with_free_func(agfx_eeee_alias_destroy);
    return state;
}

bool agfx_eeee_provider_state_destroy(AgfxEeeeProviderState *state)
{
    bool empty;

    if (!state) {
        return true;
    }
    pthread_mutex_lock(&state->lock);
    empty = g_hash_table_size(state->allocations) == 0 &&
            g_hash_table_size(state->regions) == 0 &&
            g_hash_table_size(state->published_tasks) == 0 &&
            state->retired_regions->len == 0;
    if (empty) {
        state->destroying = true;
    }
    pthread_mutex_unlock(&state->lock);
    if (!empty) {
        return false;
    }
    g_hash_table_destroy(state->published_tasks);
    g_hash_table_destroy(state->regions);
    g_hash_table_destroy(state->allocations);
    g_ptr_array_unref(state->retired_regions);
    pthread_mutex_destroy(&state->lock);
    g_free(state);
    return true;
}

static AgfxEeeeProviderState *agfx_eeee_state(void *ctx)
{
    PvgState *s = ctx;

    if (!s || !s->eeee_provider_state) {
        /* impossible partial-cutover invocation (fork: abort) */
        abort();
    }
    return s->eeee_provider_state;
}

static AgfxEeeeAllocation *agfx_eeee_allocation_exact(AgfxEeeeProviderState *state,
                                                      uint64_t address)
{
    return g_hash_table_lookup(state->allocations, &address);
}

static AgfxEeeeAllocation *agfx_eeee_allocation_containing(
    AgfxEeeeProviderState *state, uint64_t address, uint64_t length)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, state->allocations);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        AgfxEeeeAllocation *allocation = value;

        if (agfx_eeee_contains(allocation->address, allocation->length,
                               address, length)) {
            return allocation;
        }
    }
    return NULL;
}

void agfx_eeee_source_backing_ref(AgfxEeeeSourceBacking *source)
{
    g_assert_nonnull(source);
    g_assert_cmpint(pvg_atomic_fetch_inc(&source->references), >, 0);
}

void agfx_eeee_source_backing_unref(AgfxEeeeSourceBacking *source)
{
    if (!source || pvg_atomic_fetch_dec(&source->references) != 1) {
        return;
    }
    /* Unmap before dropping physical owners. */
    if (source->mapping != MAP_FAILED &&
        munmap(source->mapping, source->mapped_length)) {
        abort();
    }
    for (guint i = 0; i < source->pieces->len; ++i) {
        AgfxEeeeSourcePiece *piece = &g_array_index(source->pieces,
                                                    AgfxEeeeSourcePiece, i);

        agfx_eeee_physical_backing_unref(piece->backing);
    }
    g_array_unref(source->pieces);
    g_free(source);
}

/* The already-owned immutable pieces are the only inputs. */
static bool agfx_source_backing_map_pieces(AgfxEeeeSourceBacking *source,
                                           uint64_t alignment)
{
    uint64_t mapped_length = source->mapped_length;
    void *reservation;
    uint64_t reservation_length, aligned_address, prefix, suffix;

    reservation_length = mapped_length + alignment;
    reservation = mmap(NULL, reservation_length, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
        return false;
    }
    aligned_address = ((uint64_t)(uintptr_t)reservation + alignment - 1) &
                      ~(alignment - 1);
    prefix = aligned_address - (uint64_t)(uintptr_t)reservation;
    suffix = reservation_length - prefix - mapped_length;
    if ((prefix && munmap(reservation, prefix)) ||
        (suffix && munmap((void *)(uintptr_t)(aligned_address + mapped_length),
                          suffix))) {
        abort();
    }
    source->mapping = (void *)(uintptr_t)aligned_address;
    for (guint i = 0; i < source->pieces->len; ++i) {
        const AgfxEeeeSourcePiece *piece = &g_array_index(source->pieces,
                                                          AgfxEeeeSourcePiece, i);
        void *target = (uint8_t *)source->mapping + piece->mapping_offset;
        void *mapped = mmap(target, piece->length, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, piece->backing->fd,
                            piece->backing->file_offset + piece->region_offset);

        if (mapped == MAP_FAILED) {
            return false;
        }
        if (mapped != target) {
            abort();
        }
    }
    return true;
}

void *agfx_eeee_source_backing_pointer(const AgfxEeeeSourceBacking *source)
{
    return (uint8_t *)source->mapping + source->view_offset;
}

uint64_t agfx_eeee_source_backing_length(const AgfxEeeeSourceBacking *source)
{
    return source->length;
}

void *agfx_eeee_source_backing_allocation(const AgfxEeeeSourceBacking *source)
{
    return source->mapping;
}

uint64_t agfx_eeee_source_backing_allocation_length(const AgfxEeeeSourceBacking *source)
{
    return source->mapped_length;
}

uint64_t agfx_eeee_source_backing_visible_offset(const AgfxEeeeSourceBacking *source)
{
    return source->view_offset;
}

unsigned agfx_eeee_source_backing_interval_count(const AgfxEeeeSourceBacking *source)
{
    return source->pieces->len;
}

bool agfx_eeee_source_backing_interval(const AgfxEeeeSourceBacking *source,
                                       unsigned index,
                                       AgfxEeeeSourceInterval *interval)
{
    const AgfxEeeeSourcePiece *piece;

    if (!interval || index >= source->pieces->len) {
        return false;
    }
    piece = &g_array_index(source->pieces, AgfxEeeeSourcePiece, index);
    *interval = (AgfxEeeeSourceInterval) {
        .region = piece->backing->region,
        .fd = piece->backing->fd,
        .region_offset = piece->region_offset,
        .file_offset = piece->backing->file_offset + piece->region_offset,
        .allocation_offset = piece->mapping_offset,
        .length = piece->length,
    };
    return true;
}

/* No dirty log out of process: validates the visible range only. */
bool agfx_eeee_source_backing_mark_dirty(AgfxEeeeSourceBacking *source,
                                         uint64_t offset, uint64_t length)
{
    return source && offset <= source->length && length <= source->length - offset;
}

/* Each surviving interval owns its backing reference; retirement moves the
 * reference without dropping it under the task mutex. */
static void agfx_eeee_remove_overwritten_aliases(AgfxEeeeProviderState *state,
                                                 AgfxEeeeAllocation *allocation,
                                                 uint64_t address, uint64_t length)
{
    const uint64_t end = address + length;
    guint index = 0;

    while (index < allocation->aliases->len) {
        AgfxEeeeAlias *alias = g_ptr_array_index(allocation->aliases, index);
        const uint64_t alias_end = alias->target + alias->length;

        if (!agfx_eeee_intersects(alias->target, alias->length, address, length)) {
            ++index;
            continue;
        }
        if (alias->target < address && alias_end > end) {
            AgfxEeeeAlias *suffix = g_new(AgfxEeeeAlias, 1);

            *suffix = *alias;
            agfx_eeee_physical_backing_ref(suffix->backing);
            suffix->target = end;
            suffix->region_offset += end - alias->target;
            suffix->length = alias_end - end;
            alias->length = address - alias->target;
            g_ptr_array_insert(allocation->aliases, index + 1, suffix);
            index += 2;
            continue;
        }
        if (alias->target < address) {
            alias->length = address - alias->target;
            ++index;
            continue;
        }
        if (alias_end > end) {
            alias->region_offset += end - alias->target;
            alias->target = end;
            alias->length = alias_end - end;
            ++index;
            continue;
        }
        g_ptr_array_add(state->retired_regions,
                        g_ptr_array_steal_index(allocation->aliases, index));
    }
}

static int agfx_eeee_task_allocate(void *ctx, uint64_t *address,
                                   uint64_t length, uint32_t placement)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    void *requested = NULL;
    void *mapped;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    AgfxEeeeAllocation *allocation;
    uint64_t mapped_length;
    GPtrArray *retired;

    if (!address || !agfx_eeee_round_length(length, pvg_page_size(), &mapped_length)) {
        return 0;
    }
    switch (placement) {
    case QMU_EEEE_TASK_ALLOCATE_ANYWHERE:
        /* the caller leaves *address uninitialized */
        break;
    case QMU_EEEE_TASK_ALLOCATE_FIXED_OVERWRITE:
        if (!agfx_eeee_page_range(*address, length)) {
            return 0;
        }
        requested = (void *)(uintptr_t)*address;
        flags |= MAP_FIXED;
        break;
    default:
        abort();
    }

    if (placement == QMU_EEEE_TASK_ALLOCATE_FIXED_OVERWRITE) {
        /* MAP_FIXED only inside an allocation this provider owns. */
        pthread_mutex_lock(&state->lock);
        allocation = agfx_eeee_allocation_containing(state, *address, length);
        if (!allocation || state->destroying) {
            pthread_mutex_unlock(&state->lock);
            return 0;
        }
        mapped = mmap(requested, length, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (mapped == MAP_FAILED || mapped != requested) {
            if (mapped != MAP_FAILED) {
                abort();
            }
            pthread_mutex_unlock(&state->lock);
            return 0;
        }
        agfx_eeee_remove_overwritten_aliases(state, allocation, *address, length);
        retired = agfx_eeee_take_retired_regions(state);
        pthread_mutex_unlock(&state->lock);
        if (retired) {
            g_ptr_array_unref(retired);
        }
        return 1;
    }

    mapped = mmap(requested, mapped_length, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapped == MAP_FAILED || (requested && mapped != requested)) {
        if (mapped != MAP_FAILED) {
            munmap(mapped, mapped_length);
        }
        return 0;
    }

    pthread_mutex_lock(&state->lock);
    if (state->destroying) {
        pthread_mutex_unlock(&state->lock);
        munmap(mapped, mapped_length);
        return 0;
    }
    allocation = g_new0(AgfxEeeeAllocation, 1);
    allocation->address = (uint64_t)(uintptr_t)mapped;
    allocation->length = length;
    allocation->mapped_length = mapped_length;
    allocation->aliases = g_ptr_array_new_with_free_func(agfx_eeee_alias_destroy);
    g_hash_table_insert(state->allocations, &allocation->address, allocation);
    pthread_mutex_unlock(&state->lock);
    *address = allocation->address;
    return 1;
}

/* A refused remap is fatal in QMetal (ProviderFatal::remap): say why. */
#define PVM_REMAP_FAIL(why, err) \
    fprintf(stderr, "[apple-gfx-ml][pvm][ERROR] remap refused: %s errno=%d target=0x%" PRIx64 \
            " len=0x%" PRIx64 " source=0x%" PRIx64 "\n", (why), (err), \
            address ? *address : 0, length, source)

static int agfx_eeee_task_remap_shared_fixed_overwrite(
    void *ctx, uint64_t *address, uint64_t length, uint64_t mask,
    uint64_t source, int *current_protection, int *maximum_protection)
{
    PvgState *s = ctx;
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    PvgRegion *region;
    uint64_t region_offset = 0;
    uint64_t file_offset;
    void *mapped;
    AgfxEeeeAllocation *allocation;
    AgfxEeeeAlias *alias;
    AgfxEeeePhysicalBacking *backing;
    int saved_fd;
    const uint64_t page_size = pvg_page_size();

    if (!address || !current_protection || !maximum_protection ||
        mask != page_size - 1 || !agfx_eeee_page_range(*address, length) ||
        !agfx_eeee_page_range(source, length)) {
        PVM_REMAP_FAIL(mask != page_size - 1 ? "mask" : "alignment", 0);
        return 0;
    }
    /* QMetal holds the read section across translate/direct/RAM pointer and
     * this call: source is a guest-RAM location, never a task VA. */
    region = pvg_memory_region_from_host(s, (void *)(uintptr_t)source, &region_offset);
    if (!region || !region->map || region->fd < 0 ||
        region_offset > region->len || length > region->len - region_offset) {
        PVM_REMAP_FAIL(!region ? "source not in a DMA region" : region->fd < 0 ? "region without fd" :
                       !region->map ? "region not mapped" : "source range beyond region", 0);
        return 0;
    }
    file_offset = region->fd_offset;
    if (file_offset > UINT64_MAX - region_offset ||
        file_offset + region_offset > INT64_MAX ||
        !agfx_eeee_page_range(file_offset + region_offset, length)) {
        PVM_REMAP_FAIL("file offset", 0);
        return 0;
    }

    /* QMetal calls remap under task_lock. */
    allocation = agfx_eeee_allocation_containing(state, *address, length);
    if (!allocation || state->destroying ||
        GPOINTER_TO_UINT(g_hash_table_lookup(state->regions, region)) == 0) {
        PVM_REMAP_FAIL(!allocation ? "target not in a provider allocation" : state->destroying ? "destroying" :
                       "region not referenced by the task", 0);
        return 0;
    }
    saved_fd = fcntl(region->fd, F_DUPFD_CLOEXEC, 0);
    if (saved_fd < 0) {
        PVM_REMAP_FAIL("dup fd", errno);
        return 0;
    }
    mapped = mmap((void *)(uintptr_t)*address, length, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_FIXED, saved_fd, (off_t)(file_offset + region_offset));
    if (mapped == MAP_FAILED || mapped != (void *)(uintptr_t)*address) {
        PVM_REMAP_FAIL(mapped == MAP_FAILED ? "mmap" : "mmap moved", mapped == MAP_FAILED ? errno : 0);
        if (mapped != MAP_FAILED) {
            munmap(mapped, length);
        }
        close(saved_fd);
        return 0;
    }
    /* Keep the exact RAM owner across the provider's earlier region_unref. */
    pvg_region_ref(s, region);
    backing = g_new(AgfxEeeePhysicalBacking, 1);
    *backing = (AgfxEeeePhysicalBacking) {
        .references = 1,
        .state = s,
        .region = region,
        .fd = saved_fd,
        .file_offset = file_offset,
    };
    agfx_eeee_remove_overwritten_aliases(state, allocation, *address, length);
    alias = g_new(AgfxEeeeAlias, 1);
    *alias = (AgfxEeeeAlias) {
        .backing = backing,
        .target = *address,
        .region_offset = region_offset,
        .length = length,
    };
    g_ptr_array_add(allocation->aliases, alias);

    *current_protection = PROT_READ | PROT_WRITE;
    *maximum_protection = PROT_READ | PROT_WRITE;
    return 1;
}

static int agfx_eeee_task_protect(void *ctx, uint64_t address,
                                  uint64_t length, uint32_t protection)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    int native_protection;
    AgfxEeeeAllocation *allocation;

    switch (protection) {
    case QMU_EEEE_TASK_PROTECT_NONE:
        native_protection = PROT_NONE;
        break;
    case QMU_EEEE_TASK_PROTECT_READ_ONLY:
        native_protection = PROT_READ;
        break;
    case QMU_EEEE_TASK_PROTECT_READ_WRITE:
        native_protection = PROT_READ | PROT_WRITE;
        break;
    default:
        abort();
    }
    if (length == 0) {
        return 1;
    }
    if (!agfx_eeee_page_range(address, length)) {
        return 0;
    }
    allocation = agfx_eeee_allocation_containing(state, address, length);
    if (!allocation || state->destroying ||
        mprotect((void *)(uintptr_t)address, length, native_protection) != 0) {
        return 0;
    }
    return 1;
}

static int agfx_eeee_task_deallocate(void *ctx, uint64_t address, uint64_t length)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    AgfxEeeeAllocation *allocation;

    if (!agfx_eeee_range(address, length)) {
        return -1;
    }
    pthread_mutex_lock(&state->lock);
    allocation = agfx_eeee_allocation_exact(state, address);
    if (!allocation || allocation->length != length || state->destroying ||
        munmap((void *)(uintptr_t)address, allocation->mapped_length) != 0) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }
    g_hash_table_steal(state->allocations, &address);
    pthread_mutex_unlock(&state->lock);
    /* alias references are released outside the task mutex */
    agfx_eeee_allocation_destroy(allocation);
    return 0;
}

static uint64_t agfx_eeee_task_host_page_size(void *ctx)
{
    (void)agfx_eeee_state(ctx);
    return pvg_page_size();
}

static void agfx_eeee_task_rcu_enter(void *ctx)
{
    (void)agfx_eeee_state(ctx);
    pvg_memory_read_enter(ctx);
}

static void agfx_eeee_task_rcu_leave(void *ctx)
{
    (void)agfx_eeee_state(ctx);
    pvg_memory_read_leave(ctx);
}

static void agfx_eeee_task_lock(void *ctx)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);

    pthread_mutex_lock(&state->lock);
}

static void agfx_eeee_task_unlock(void *ctx)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    GPtrArray *retired = agfx_eeee_take_retired_regions(state);

    pthread_mutex_unlock(&state->lock);
    if (retired) {
        g_ptr_array_unref(retired);
    }
}

/* fork: address_space_translate under the caller's RCU section */
static int agfx_eeee_task_translate(void *ctx, uint64_t gpa, uint64_t length,
                                    int write, void **region,
                                    uint64_t *region_offset, uint64_t *covered)
{
    PvgState *s = ctx;
    PvgRegion *r;
    uint64_t offset;

    (void)agfx_eeee_state(ctx);
    (void)write;
    if (!region || !region_offset || !covered || length == 0) {
        return 0;
    }
    pvg_memory_read_enter(s);
    r = pvg_memory_lookup_locked(s, gpa);
    if (r) {
        offset = gpa - r->iova;
        *region = r;
        *region_offset = offset;
        *covered = MIN(length, r->len - offset);
    }
    pvg_memory_read_leave(s);
    if (!r) {
        pvg_memory_refuse(s, "eeee_task_translate", gpa, length, "not in a DMA region");
    }
    return r != NULL;
}

static int agfx_eeee_task_direct(void *ctx, void *region, int write)
{
    PvgRegion *r = region;

    (void)agfx_eeee_state(ctx);
    return r && r->map && (!write || (r->prot & PROT_WRITE));
}

static void *agfx_eeee_task_ram_pointer(void *ctx, void *region)
{
    PvgRegion *r = region;

    (void)agfx_eeee_state(ctx);
    return r ? r->map : NULL;
}

static void agfx_eeee_task_region_ref(void *ctx, void *opaque_region)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    PvgRegion *region = opaque_region;
    guint count;

    if (!region) {
        abort();
    }
    pvg_region_ref(ctx, region);
    /* called under task_lock right before the first remap */
    count = GPOINTER_TO_UINT(g_hash_table_lookup(state->regions, region));
    if (count == G_MAXUINT || state->destroying) {
        pvg_region_unref(ctx, region);
        abort();
    }
    g_hash_table_insert(state->regions, region, GUINT_TO_POINTER(count + 1));
}

static void agfx_eeee_task_region_unref(void *ctx, void *opaque_region)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    PvgRegion *region = opaque_region;
    guint count;

    if (!region) {
        abort();
    }
    pthread_mutex_lock(&state->lock);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(state->regions, region));
    if (count == 0) {
        pthread_mutex_unlock(&state->lock);
        abort();
    }
    if (count == 1) {
        g_hash_table_remove(state->regions, region);
    } else {
        g_hash_table_insert(state->regions, region, GUINT_TO_POINTER(count - 1));
    }
    pthread_mutex_unlock(&state->lock);
    pvg_region_unref(ctx, region);
}

static void agfx_eeee_task_publish(void *ctx, void *task)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);

    if (!task) {
        abort();
    }
    /* called while QMetal's provider task lock is held */
    if (state->destroying || g_hash_table_contains(state->published_tasks, task)) {
        abort();
    }
    g_hash_table_add(state->published_tasks, task);
}

static void agfx_eeee_task_remove(void *ctx, void *task)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);

    if (!task || !g_hash_table_remove(state->published_tasks, task)) {
        abort();
    }
}

/* fork: memory_region_set_dirty per alias. No dirty log here: the range
 * check is kept (an uncovered host write is a provider contract break). */
static void agfx_eeee_task_host_write(void *ctx, uint64_t address, uint64_t length)
{
    AgfxEeeeProviderState *state = agfx_eeee_state(ctx);
    AgfxEeeeAllocation *allocation;

    if (!agfx_eeee_range(address, length)) {
        abort();
    }
    pthread_mutex_lock(&state->lock);
    allocation = agfx_eeee_allocation_containing(state, address, length);
    if (!allocation || state->destroying) {
        pthread_mutex_unlock(&state->lock);
        abort();
    }
    pthread_mutex_unlock(&state->lock);
}

static void agfx_eeee_task_fatal(void *ctx, uint32_t reason)
{
    (void)agfx_eeee_state(ctx);
    switch (reason) {
    case QMU_EEEE_TASK_FATAL_ALLOCATION:
    case QMU_EEEE_TASK_FATAL_REMAP:
    case QMU_EEEE_TASK_FATAL_UNMAP:
        fprintf(stderr, "apple-gfx-ml: EEEE process-VM provider fatal reason %u\n", reason);
        fflush(stderr);
        abort();
    default:
        abort();
    }
}

void agfx_eeee_provider_fill_callbacks(qmu_extended_callbacks *callbacks)
{
    if (!callbacks) {
        abort();
    }
    callbacks->eeee_task_allocate = agfx_eeee_task_allocate;
    callbacks->eeee_task_remap_shared_fixed_overwrite =
        agfx_eeee_task_remap_shared_fixed_overwrite;
    callbacks->eeee_task_protect = agfx_eeee_task_protect;
    callbacks->eeee_task_deallocate = agfx_eeee_task_deallocate;
    callbacks->eeee_task_host_page_size = agfx_eeee_task_host_page_size;
    callbacks->eeee_task_rcu_enter = agfx_eeee_task_rcu_enter;
    callbacks->eeee_task_rcu_leave = agfx_eeee_task_rcu_leave;
    callbacks->eeee_task_lock = agfx_eeee_task_lock;
    callbacks->eeee_task_unlock = agfx_eeee_task_unlock;
    callbacks->eeee_task_translate = agfx_eeee_task_translate;
    callbacks->eeee_task_direct = agfx_eeee_task_direct;
    callbacks->eeee_task_ram_pointer = agfx_eeee_task_ram_pointer;
    callbacks->eeee_task_region_ref = agfx_eeee_task_region_ref;
    callbacks->eeee_task_region_unref = agfx_eeee_task_region_unref;
    callbacks->eeee_task_publish = agfx_eeee_task_publish;
    callbacks->eeee_task_remove = agfx_eeee_task_remove;
    callbacks->eeee_task_host_write = agfx_eeee_task_host_write;
    callbacks->eeee_task_fatal = agfx_eeee_task_fatal;
}

/* ---------------------------------------------- native CPU facilities context
 * Fork: struct AgfxEeeeNativeContext in apple-gfx-ml.c. The root owns one
 * reference; each acquired facilities object owns another. */
struct AgfxEeeeNativeContext {
    PvgState *state;
    pthread_mutex_t lock;
    pthread_cond_t released;
    unsigned references;
    bool acquiring_stopped;
    qmu_extended_callbacks callbacks;
};

AgfxEeeeNativeContext *agfx_eeee_native_context_create(PvgState *s)
{
    AgfxEeeeNativeContext *context;

    if (!s || s->eeee_native_context || s->eeee_provider_state || s->exiting) {
        return NULL;
    }
    context = g_new0(AgfxEeeeNativeContext, 1);
    context->state = s;
    pthread_mutex_init(&context->lock, NULL);
    pthread_cond_init(&context->released, NULL);
    context->references = 1;
    context->callbacks = (qmu_extended_callbacks) {
        .abi_magic = QMU_EXTENDED_CALLBACKS_ABI_MAGIC,
        .abi_version = QMU_EXTENDED_ABI_VERSION,
        .struct_size = sizeof(qmu_extended_callbacks),
        .user_ctx = s,
        .map_gpa = pvg_map_gpa,
        .unmap_gpa = pvg_unmap_gpa,
        .mark_mapped_memory_dirty = pvg_mark_mapped_memory_dirty,
        .read_memory = pvg_read_memory,
        .read_ram_memory = pvg_read_ram_memory,
    };
    s->eeee_provider_state = agfx_eeee_provider_state_create();
    agfx_eeee_provider_fill_callbacks(&context->callbacks);
    s->eeee_native_context = context;
    return context;
}

bool agfx_eeee_native_context_acquire(AgfxEeeeNativeContext *context,
                                      qmu_extended_callbacks *callbacks)
{
    bool acquired = false;

    if (!context || !callbacks) {
        return false;
    }
    pthread_mutex_lock(&context->lock);
    if (!context->acquiring_stopped) {
        g_assert_cmpuint(context->references, >, 0);
        context->references++;
        *callbacks = context->callbacks;
        acquired = true;
    }
    pthread_mutex_unlock(&context->lock);
    return acquired;
}

void agfx_eeee_native_context_release(void *opaque)
{
    AgfxEeeeNativeContext *context = opaque;

    pthread_mutex_lock(&context->lock);
    g_assert_cmpuint(context->references, >, 1);
    context->references--;
    pthread_cond_broadcast(&context->released);
    pthread_mutex_unlock(&context->lock);
}

void agfx_eeee_native_context_stop_acquiring(AgfxEeeeNativeContext *context)
{
    if (context) {
        pthread_mutex_lock(&context->lock);
        context->acquiring_stopped = true;
        pthread_mutex_unlock(&context->lock);
    }
}

bool agfx_eeee_native_context_retire(AgfxEeeeNativeContext *context)
{
    PvgState *s;

    if (!context) {
        return true;
    }
    agfx_eeee_native_context_stop_acquiring(context);
    pthread_mutex_lock(&context->lock);
    while (context->references != 1) {
        pthread_cond_wait(&context->released, &context->lock);
    }
    pthread_mutex_unlock(&context->lock);
    s = context->state;
    if (!agfx_eeee_provider_state_destroy(s->eeee_provider_state)) {
        return false;   /* busy owners preserved, never a fake teardown */
    }
    s->eeee_provider_state = NULL;
    s->eeee_native_context = NULL;
    pthread_cond_destroy(&context->released);
    pthread_mutex_destroy(&context->lock);
    g_free(context);
    return true;
}

/* ----------------------------------------- ordinary physical capture V2
 * Fork: AgfxOrdinaryPhysicalContext (apple-gfx-ml.c) and the capture /
 * describe / dirty / release / compare leaves (apple-gfx-eeee-process-vm.c). */
struct AgfxOrdinaryPhysicalOrigin {
    int references;
    const void *context_identity;
    bool stopped;
};

static AgfxOrdinaryPhysicalOrigin *agfx_ordinary_physical_origin_new(const void *context)
{
    AgfxOrdinaryPhysicalOrigin *origin = g_new0(AgfxOrdinaryPhysicalOrigin, 1);

    origin->references = 1;
    origin->context_identity = context;
    return origin;
}

static AgfxOrdinaryPhysicalOrigin *agfx_ordinary_physical_origin_ref(
    AgfxOrdinaryPhysicalOrigin *origin)
{
    pvg_atomic_inc(&origin->references);
    return origin;
}

static void agfx_ordinary_physical_origin_unref(AgfxOrdinaryPhysicalOrigin *origin)
{
    if (origin && pvg_atomic_fetch_dec(&origin->references) == 1) {
        g_free(origin);
    }
}

struct AgfxOrdinaryPhysicalContext {
    PvgState *state;
    AgfxOrdinaryPhysicalOrigin *origin;
    pthread_mutex_t lock;
    pthread_cond_t released;
    unsigned references;
    unsigned captures;
    bool stopped;
};

static void agfx_ordinary_physical_capture(void *, const struct qmu_ordinary_physical_request_v2 *,
                                           void **);
static bool agfx_ordinary_physical_describe(const void *, struct qmu_ordinary_physical_view_v2 *);
static bool agfx_ordinary_physical_dirty(void *, uint64_t, uint64_t);
static void agfx_ordinary_physical_release_span(void *);

AgfxOrdinaryPhysicalContext *agfx_ordinary_physical_context_create(PvgState *s)
{
    AgfxOrdinaryPhysicalContext *context;

    if (!s || s->ordinary_physical_context || s->exiting) {
        return NULL;
    }
    context = g_new0(AgfxOrdinaryPhysicalContext, 1);
    context->state = s;
    context->origin = agfx_ordinary_physical_origin_new(context);
    pthread_mutex_init(&context->lock, NULL);
    pthread_cond_init(&context->released, NULL);
    context->references = 1;
    s->ordinary_physical_context = context;
    return context;
}

bool agfx_ordinary_physical_context_acquire(AgfxOrdinaryPhysicalContext *context,
    struct qmu_ordinary_physical_handoff_v2 *handoff)
{
    bool acquired = false;

    if (!context || !handoff || handoff->acquired_context) {
        return false;
    }
    pthread_mutex_lock(&context->lock);
    if (!context->stopped) {
        context->references++;
        *handoff = (struct qmu_ordinary_physical_handoff_v2) {
            .version = QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_VERSION,
            .struct_size = sizeof(*handoff),
            .acquired_context = context,
            .memory_ctx_identity = context->state,
            .stop = agfx_ordinary_physical_context_stop,
            .release_context = agfx_ordinary_physical_context_release,
            .capture = agfx_ordinary_physical_capture,
            .describe = agfx_ordinary_physical_describe,
            .mark_dirty = agfx_ordinary_physical_dirty,
            .release_span = agfx_ordinary_physical_release_span,
        };
        acquired = true;
    }
    pthread_mutex_unlock(&context->lock);
    return acquired;
}

bool agfx_ordinary_physical_context_stop(void *opaque)
{
    AgfxOrdinaryPhysicalContext *context = opaque;

    if (!context) {
        return false;
    }
    pthread_mutex_lock(&context->lock);
    context->stopped = true;
    pvg_atomic_set(&context->origin->stopped, true);
    pthread_mutex_unlock(&context->lock);
    return true;
}

void agfx_ordinary_physical_context_release(void *opaque)
{
    AgfxOrdinaryPhysicalContext *context = opaque;

    pthread_mutex_lock(&context->lock);
    g_assert_cmpuint(context->references, >, 1);
    context->references--;
    pthread_cond_broadcast(&context->released);
    pthread_mutex_unlock(&context->lock);
}

static bool agfx_ordinary_physical_capture_enter(void *opaque)
{
    AgfxOrdinaryPhysicalContext *context = opaque;
    bool entered = false;

    if (!context) {
        return false;
    }
    pthread_mutex_lock(&context->lock);
    if (!context->stopped && context->references > 1) {
        context->captures++;
        entered = true;
    }
    pthread_mutex_unlock(&context->lock);
    return entered;
}

static void agfx_ordinary_physical_capture_leave(void *opaque)
{
    AgfxOrdinaryPhysicalContext *context = opaque;

    pthread_mutex_lock(&context->lock);
    g_assert_cmpuint(context->captures, >, 0);
    context->captures--;
    pthread_cond_broadcast(&context->released);
    pthread_mutex_unlock(&context->lock);
}

void agfx_ordinary_physical_context_retire(AgfxOrdinaryPhysicalContext *context)
{
    PvgState *s;

    if (!context) {
        return;
    }
    agfx_ordinary_physical_context_stop(context);
    pthread_mutex_lock(&context->lock);
    while (context->references != 1 || context->captures != 0) {
        pthread_cond_wait(&context->released, &context->lock);
    }
    pthread_mutex_unlock(&context->lock);
    s = context->state;
    s->ordinary_physical_context = NULL;
    pthread_cond_destroy(&context->released);
    pthread_mutex_destroy(&context->lock);
    agfx_ordinary_physical_origin_unref(context->origin);
    g_free(context);
}

typedef struct AgfxOrdinaryPhysicalSpan {
    AgfxOrdinaryPhysicalOrigin *origin;
    AgfxEeeeSourceBacking *source;
    AgfxEeeeSourceDmaBuf *dma_export;
    AgfxEeeeDmaBufFailure export_failure;
    uint32_t task_id, resource_id;
    uint64_t task_epoch, object_epoch, source_va;
    bool complete;
} AgfxOrdinaryPhysicalSpan;

/* Admitted inputs are guest-physical RAM addresses: without a vIOMMU the
 * client's DMA regions are GPA-indexed (the fork's IOMMU domain check has
 * nothing to inspect here). */
static void agfx_ordinary_physical_capture(void *opaque,
    const struct qmu_ordinary_physical_request_v2 *request, void **output)
{
    AgfxOrdinaryPhysicalContext *context = opaque;
    PvgState *s;
    const uint64_t page = pvg_page_size();
    uint64_t position = 0;
    AgfxOrdinaryPhysicalSpan *span;
    AgfxEeeeSourceBacking *source;

    if (!output || *output || !request || !context ||
        request->version != QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_VERSION ||
        request->struct_size != sizeof(*request) || !page ||
        !request->pieces || !request->piece_count ||
        request->piece_count > G_MAXUINT ||
        request->piece_count > SIZE_MAX / sizeof(AgfxEeeeSourcePiece) ||
        !request->allocation_length || !request->literal_length ||
        request->allocation_length > SIZE_MAX ||
        request->allocation_alignment < page ||
        (request->allocation_alignment & (request->allocation_alignment - 1)) ||
        request->allocation_length > SIZE_MAX - request->allocation_alignment ||
        request->allocation_length % page ||
        request->visible_offset > request->allocation_length ||
        request->literal_length > request->allocation_length - request->visible_offset) {
        return; /* no physical effect or owner was acquired */
    }
    for (uint64_t i = 0; i < request->piece_count; ++i) {
        const struct qmu_ordinary_physical_piece_v2 *piece = &request->pieces[i];

        if (piece->logical_offset != position || !piece->mapped_pointer ||
            !agfx_eeee_page_range(piece->gpa, piece->length) ||
            (uintptr_t)piece->mapped_pointer % page ||
            piece->length > request->allocation_length - position) {
            return;
        }
        position += piece->length;
    }
    if (position != request->allocation_length ||
        !agfx_ordinary_physical_capture_enter(context)) {
        return;
    }
    s = context->state;
    span = g_new0(AgfxOrdinaryPhysicalSpan, 1);
    span->origin = agfx_ordinary_physical_origin_ref(context->origin);
    span->export_failure = (AgfxEeeeDmaBufFailure) { AGFX_DMABUF_NONE, 0, UINT_MAX };
    source = g_new0(AgfxEeeeSourceBacking, 1);
    source->references = 1;
    source->mapping = MAP_FAILED;
    source->mapped_length = request->allocation_length;
    source->view_offset = request->visible_offset;
    source->length = request->literal_length;
    source->pieces = g_array_sized_new(false, false, sizeof(AgfxEeeeSourcePiece),
                                       request->piece_count);
    span->source = source;
    span->task_id = request->task_id;
    span->resource_id = request->resource_id;
    span->task_epoch = request->task_epoch;
    span->object_epoch = request->object_epoch;
    span->source_va = request->source_va;   /* provenance only */
    *output = span; /* SAME partial owner before the first fd/mapping effect */

    pvg_memory_read_enter(s);
    for (uint64_t i = 0; i < request->piece_count; ++i) {
        const struct qmu_ordinary_physical_piece_v2 *piece = &request->pieces[i];
        PvgRegion *region = pvg_memory_lookup_locked(s, piece->gpa);
        uint64_t translated_offset, file_offset;
        AgfxEeeePhysicalBacking *backing;
        AgfxEeeeSourcePiece saved;

        if (!region || !region->map || region->fd < 0 ||
            !(region->prot & PROT_WRITE)) {
            pvg_memory_refuse(s, "ordinary_physical_capture", piece->gpa, piece->length,
                              region ? "DMA region has no writable fd mapping" :
                              "not in a DMA region");
            goto partial;
        }
        translated_offset = piece->gpa - region->iova;
        if (piece->length > region->len - translated_offset ||
            (const void *)(region->map + translated_offset) != piece->mapped_pointer) {
            pvg_memory_refuse(s, "ordinary_physical_capture", piece->gpa, piece->length,
                              "piece is not one mapping of its DMA region");
            goto partial;
        }
        file_offset = region->fd_offset;
        if (file_offset > UINT64_MAX - translated_offset ||
            file_offset + translated_offset > INT64_MAX ||
            piece->length > INT64_MAX - (file_offset + translated_offset) ||
            !agfx_eeee_page_range(file_offset + translated_offset, piece->length)) {
            goto partial;
        }
        backing = g_new0(AgfxEeeePhysicalBacking, 1);
        backing->references = 1;
        backing->state = s;
        backing->fd = -1;
        backing->file_offset = file_offset;
        saved = (AgfxEeeeSourcePiece) {
            .backing = backing,
            .region_offset = translated_offset,
            .mapping_offset = piece->logical_offset,
            .length = piece->length,
        };
        g_array_append_val(source->pieces, saved);
        pvg_region_ref(s, region);
        backing->region = region;
        backing->fd = fcntl(region->fd, F_DUPFD_CLOEXEC, 0);
        if (backing->fd < 0) {
            goto partial;
        }
    }
    pvg_memory_read_leave(s);
    if (agfx_source_backing_map_pieces(source, request->allocation_alignment)) {
        span->dma_export = agfx_eeee_source_dmabuf_create(source, &span->export_failure);
        span->complete = span->dma_export &&
            agfx_eeee_source_dmabuf_fd(span->dma_export) >= 0;
        if (!span->complete) {
            pvg_log(s, "[apple-gfx-ml][memory][ERROR] udmabuf export failed stage=%d "
                    "errno=%d interval=%u task=%u resource=%u length=0x%" PRIx64 "\n",
                    (int)span->export_failure.stage, span->export_failure.error_no,
                    span->export_failure.interval, span->task_id, span->resource_id,
                    request->allocation_length);
        }
    }
    agfx_ordinary_physical_capture_leave(context);
    return;
partial:
    pvg_memory_read_leave(s);
    agfx_ordinary_physical_capture_leave(context);
    /* describe=false; the caller still owns this exact partial span */
}

static bool agfx_ordinary_physical_describe(const void *opaque,
    struct qmu_ordinary_physical_view_v2 *view)
{
    const AgfxOrdinaryPhysicalSpan *span = opaque;

    if (!span || !view || !span->complete || !span->dma_export ||
        agfx_eeee_source_dmabuf_fd(span->dma_export) < 0) {
        return false;
    }
    *view = (struct qmu_ordinary_physical_view_v2) {
        .allocation = agfx_eeee_source_backing_allocation(span->source),
        .allocation_length = agfx_eeee_source_backing_allocation_length(span->source),
        .visible_pointer = agfx_eeee_source_backing_pointer(span->source),
        .visible_offset = agfx_eeee_source_backing_visible_offset(span->source),
        .visible_length = agfx_eeee_source_backing_length(span->source),
        .dma_buf_fd = agfx_eeee_source_dmabuf_fd(span->dma_export),
        .reserved = 0,
    };
    return true;
}

static bool agfx_ordinary_physical_dirty(void *opaque, uint64_t offset, uint64_t length)
{
    AgfxOrdinaryPhysicalSpan *span = opaque;

    return span && span->complete &&
        agfx_eeee_source_backing_mark_dirty(span->source, offset, length);
}

static void agfx_ordinary_physical_release_span(void *opaque)
{
    AgfxOrdinaryPhysicalSpan *span = opaque;

    if (span) {
        /* export first; its source reference returns before ours */
        agfx_eeee_source_dmabuf_unref(span->dma_export);
        agfx_eeee_source_backing_unref(span->source);
        agfx_ordinary_physical_origin_unref(span->origin);
        g_free(span);
    }
}

typedef struct AgfxOrdinaryPhysicalInterval {
    dev_t device;
    ino_t inode;
    uint64_t first, length;
} AgfxOrdinaryPhysicalInterval;

static const struct stat *agfx_ordinary_physical_stat(
    const AgfxEeeePhysicalBacking *backing, GHashTable *stats)
{
    struct stat *st = g_hash_table_lookup(stats, backing);

    if (st) {
        return st;
    }
    st = g_new(struct stat, 1);
    if (fstat(backing->fd, st) != 0 || !S_ISREG(st->st_mode) || st->st_size < 0) {
        g_free(st);
        return NULL;
    }
    g_hash_table_insert(stats, (void *)backing, st);
    return st;
}

static gint agfx_ordinary_physical_interval_order(gconstpointer left, gconstpointer right)
{
    const AgfxOrdinaryPhysicalInterval *a = left, *b = right;

    if (a->device != b->device) {
        return a->device < b->device ? -1 : 1;
    }
    if (a->inode != b->inode) {
        return a->inode < b->inode ? -1 : 1;
    }
    return a->first < b->first ? -1 : a->first != b->first;
}

static void agfx_ordinary_physical_normalize(GArray *ranges)
{
    unsigned written = 0;

    g_array_sort(ranges, agfx_ordinary_physical_interval_order);
    for (unsigned i = 0; i < ranges->len; ++i) {
        AgfxOrdinaryPhysicalInterval current =
            g_array_index(ranges, AgfxOrdinaryPhysicalInterval, i);

        if (written) {
            AgfxOrdinaryPhysicalInterval *last =
                &g_array_index(ranges, AgfxOrdinaryPhysicalInterval, written - 1);

            if (last->device == current.device && last->inode == current.inode &&
                current.first <= last->first + last->length) {
                last->length = MAX(last->first + last->length,
                                   current.first + current.length) - last->first;
                continue;
            }
        }
        g_array_index(ranges, AgfxOrdinaryPhysicalInterval, written++) = current;
    }
    g_array_set_size(ranges, written);
}

static bool agfx_ordinary_physical_ranges(const AgfxOrdinaryPhysicalSpan *span,
    uint64_t offset, uint64_t length, GHashTable *stats, GArray *ranges)
{
    uint64_t covered = 0, first, end;

    if (!span || !span->complete || !span->source ||
        offset > span->source->length || length > span->source->length - offset ||
        offset > UINT64_MAX - span->source->view_offset) {
        return false;
    }
    first = span->source->view_offset + offset;
    if (length > UINT64_MAX - first) {
        return false;
    }
    end = first + length;
    for (unsigned i = 0; i < span->source->pieces->len; ++i) {
        const AgfxEeeeSourcePiece *piece =
            &g_array_index(span->source->pieces, AgfxEeeeSourcePiece, i);
        uint64_t begin, limit, file_first;
        const struct stat *st;

        if (!piece->backing || piece->length > UINT64_MAX - piece->mapping_offset) {
            return false;
        }
        begin = MAX(first, piece->mapping_offset);
        limit = MIN(end, piece->mapping_offset + piece->length);
        if (begin >= limit) {
            continue;
        }
        if (piece->region_offset > UINT64_MAX - piece->backing->file_offset) {
            return false;
        }
        file_first = piece->backing->file_offset + piece->region_offset;
        if (begin - piece->mapping_offset > UINT64_MAX - file_first) {
            return false;
        }
        file_first += begin - piece->mapping_offset;
        if (limit - begin > UINT64_MAX - file_first ||
            limit - begin > length - covered ||
            !(st = agfx_ordinary_physical_stat(piece->backing, stats)) ||
            file_first + (limit - begin) > (uint64_t)st->st_size) {
            return false;
        }
        AgfxOrdinaryPhysicalInterval range = {
            .device = st->st_dev, .inode = st->st_ino,
            .first = file_first, .length = limit - begin,
        };
        g_array_append_val(ranges, range);
        covered += range.length;
    }
    if (covered != length) {
        return false;
    }
    agfx_ordinary_physical_normalize(ranges);
    return true;
}

uint32_t agfx_ordinary_physical_compare(const void *context,
    const void *left_opaque, uint64_t left_offset, uint64_t left_length,
    const void *right_opaque, uint64_t right_offset, uint64_t right_length)
{
    const AgfxOrdinaryPhysicalSpan *left = left_opaque, *right = right_opaque;
    uint32_t result = QMU_ORDINARY_PHYSICAL_COMPARISON_UNAVAILABLE;
    GHashTable *stats;
    GArray *a, *b;

    if (!context || !left || !right || !left->origin ||
        left->origin != right->origin ||
        left->origin->context_identity != context ||
        pvg_atomic_read(&left->origin->stopped)) {
        return result;
    }
    stats = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    a = g_array_new(false, false, sizeof(AgfxOrdinaryPhysicalInterval));
    b = g_array_new(false, false, sizeof(AgfxOrdinaryPhysicalInterval));
    if (!agfx_ordinary_physical_ranges(left, left_offset, left_length, stats, a) ||
        !agfx_ordinary_physical_ranges(right, right_offset, right_length, stats, b)) {
        goto done;
    }
    result = QMU_ORDINARY_PHYSICAL_COMPARISON_DISJOINT;
    for (unsigned i = 0, j = 0; i < a->len && j < b->len;) {
        const AgfxOrdinaryPhysicalInterval *x =
            &g_array_index(a, AgfxOrdinaryPhysicalInterval, i);
        const AgfxOrdinaryPhysicalInterval *y =
            &g_array_index(b, AgfxOrdinaryPhysicalInterval, j);

        if (x->device != y->device) {
            if (x->device < y->device) { ++i; } else { ++j; }
        } else if (x->inode != y->inode) {
            if (x->inode < y->inode) { ++i; } else { ++j; }
        } else if (x->first + x->length <= y->first) {
            ++i;
        } else if (y->first + y->length <= x->first) {
            ++j;
        } else {
            result = QMU_ORDINARY_PHYSICAL_COMPARISON_OVERLAP;
            break;
        }
    }
done:
    g_array_unref(a);
    g_array_unref(b);
    g_hash_table_unref(stats);
    return result;
}
