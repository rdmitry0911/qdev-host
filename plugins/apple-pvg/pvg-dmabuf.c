/*
 * apple-pvg plugin: udmabuf export of a saved source backing.
 *
 * Port of hw/display/apple-gfx-eeee-source-dmabuf.c (QEMU fork c43589029b),
 * unchanged in policy: the pieces must be page aligned, every file must carry
 * F_SEAL_SHRINK and no F_SEAL_WRITE, and one UDMABUF_CREATE_LIST covers the
 * whole allocation. The fds are the plugin's dups of the client's DMA region
 * fds (QEMU memory-backend-memfd, seal=on).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pvg-process-vm.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/udmabuf.h>

struct AgfxEeeeSourceDmaBuf {
    int references;
    int fd;
    AgfxEeeeSourceBacking *source;
};

void agfx_eeee_source_dmabuf_ref(AgfxEeeeSourceDmaBuf *export)
{
    g_assert_nonnull(export);
    g_assert_cmpint(pvg_atomic_fetch_inc(&export->references), >, 0);
}

void agfx_eeee_source_dmabuf_unref(AgfxEeeeSourceDmaBuf *export)
{
    if (!export || pvg_atomic_fetch_dec(&export->references) != 1) {
        return;
    }
    if (export->fd >= 0) {
        close(export->fd);
    }
    agfx_eeee_source_backing_unref(export->source);
    g_free(export);
}

AgfxEeeeSourceDmaBuf *agfx_eeee_source_dmabuf_create(
    AgfxEeeeSourceBacking *source, AgfxEeeeDmaBufFailure *failure)
{
    AgfxEeeeSourceDmaBuf *export = NULL;
    struct udmabuf_create_list *list = NULL;
    AgfxEeeeDmaBufFailure result = { AGFX_DMABUF_NONE, 0, UINT_MAX };
    uint64_t allocation, visible, length, position = 0;
    const uint64_t page = pvg_page_size();
    size_t list_bytes;
    unsigned count;
    int control = -1;

    if (!source) {
        result.stage = AGFX_DMABUF_SOURCE_LAYOUT;
        goto fail;
    }
    export = g_try_malloc0(sizeof(*export));
    if (!export) {
        result = (AgfxEeeeDmaBufFailure) { AGFX_DMABUF_ALLOCATION, ENOMEM, UINT_MAX };
        goto fail;
    }
    export->references = 1;
    export->fd = -1;
    agfx_eeee_source_backing_ref(source);
    export->source = source;
    allocation = agfx_eeee_source_backing_allocation_length(source);
    visible = agfx_eeee_source_backing_visible_offset(source);
    length = agfx_eeee_source_backing_length(source);
    count = agfx_eeee_source_backing_interval_count(source);
    G_STATIC_ASSERT(sizeof(count) <= sizeof(list->count));
    if (!page || !allocation || allocation % page || !length ||
        visible > allocation || length > allocation - visible || !count ||
        __builtin_mul_overflow((size_t)count, sizeof(list->list[0]), &list_bytes) ||
        __builtin_add_overflow(list_bytes, sizeof(*list), &list_bytes)) {
        result.stage = AGFX_DMABUF_SOURCE_LAYOUT;
        goto fail;
    }
    list = g_try_malloc0(list_bytes);
    if (!list) {
        result = (AgfxEeeeDmaBufFailure) { AGFX_DMABUF_ALLOCATION, ENOMEM, UINT_MAX };
        goto fail;
    }
    for (unsigned i = 0; i < count; ++i) {
        AgfxEeeeSourceInterval interval;
        struct stat file;
        int seals;

        result.interval = i;
        if (!agfx_eeee_source_backing_interval(source, i, &interval) ||
            interval.fd < 0 || !interval.length ||
            interval.allocation_offset != position ||
            interval.allocation_offset % page || interval.file_offset % page ||
            interval.length % page || position > allocation ||
            interval.length > allocation - position) {
            result.stage = AGFX_DMABUF_SOURCE_LAYOUT;
            goto fail;
        }
        seals = fcntl(interval.fd, F_GET_SEALS);
        if (seals < 0) {
            result.stage = AGFX_DMABUF_FILE_SEALS;
            result.error_no = errno;
            goto fail;
        }
        if (!(seals & F_SEAL_SHRINK) || (seals & F_SEAL_WRITE)) {
            result.stage = AGFX_DMABUF_SEAL_POLICY;
            goto fail;
        }
        /* Shrink is prohibited before the extent is observed. */
        if (fstat(interval.fd, &file) < 0) {
            result.stage = AGFX_DMABUF_FILE_STAT;
            result.error_no = errno;
            goto fail;
        }
        if (file.st_size < 0 || interval.file_offset > (uint64_t)file.st_size ||
            interval.length > (uint64_t)file.st_size - interval.file_offset) {
            result.stage = AGFX_DMABUF_FILE_EXTENT;
            goto fail;
        }
        list->list[i].memfd = interval.fd;
        list->list[i].offset = interval.file_offset;
        list->list[i].size = interval.length;
        position += interval.length;
    }
    if (position != allocation) {
        result.stage = AGFX_DMABUF_SOURCE_LAYOUT;
        goto fail;
    }
    list->count = count;
    list->flags = UDMABUF_FLAGS_CLOEXEC;
    result.interval = UINT_MAX;
    control = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (control < 0) {
        result.stage = AGFX_DMABUF_CONTROL_OPEN;
        result.error_no = errno;
        goto fail;
    }
    export->fd = ioctl(control, UDMABUF_CREATE_LIST, list);
    if (export->fd < 0) {
        result.stage = AGFX_DMABUF_CREATE_LIST;
        result.error_no = errno;
        goto fail;
    }
    close(control);
    g_free(list);
    if (failure) {
        *failure = (AgfxEeeeDmaBufFailure) { AGFX_DMABUF_NONE, 0, UINT_MAX };
    }
    return export;

fail:
    if (control >= 0) {
        close(control);
    }
    g_free(list);
    agfx_eeee_source_dmabuf_unref(export);
    if (failure) {
        *failure = result;
    }
    errno = result.error_no;
    return NULL;
}

int agfx_eeee_source_dmabuf_fd(const AgfxEeeeSourceDmaBuf *export)
{
    return export->fd;
}
