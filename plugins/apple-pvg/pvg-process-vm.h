/*
 * apple-pvg plugin: saved physical backings and their dma-buf export.
 *
 * Port of hw/display/apple-gfx-eeee-process-vm.h and
 * apple-gfx-eeee-source-dmabuf.h (QEMU fork c43589029b). A source backing is
 * a private CPU mapping assembled from pieces of guest RAM files (MAP_SHARED
 * | MAP_FIXED of the saved region fds), owning one reference to each piece's
 * region; a dma-buf export is one UDMABUF_CREATE_LIST over the same pieces.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef APPLE_PVG_PROCESS_VM_H
#define APPLE_PVG_PROCESS_VM_H

#include "pvg.h"

typedef struct AgfxEeeeSourceBacking AgfxEeeeSourceBacking;

void agfx_eeee_source_backing_ref(AgfxEeeeSourceBacking *source);
void agfx_eeee_source_backing_unref(AgfxEeeeSourceBacking *source);
void *agfx_eeee_source_backing_pointer(const AgfxEeeeSourceBacking *source);
uint64_t agfx_eeee_source_backing_length(const AgfxEeeeSourceBacking *source);
void *agfx_eeee_source_backing_allocation(const AgfxEeeeSourceBacking *source);
uint64_t agfx_eeee_source_backing_allocation_length(const AgfxEeeeSourceBacking *source);
uint64_t agfx_eeee_source_backing_visible_offset(const AgfxEeeeSourceBacking *source);

typedef struct AgfxEeeeSourceInterval {
    /* Borrowed identities/descriptor; do not close or unref. */
    void *region;
    int fd;
    uint64_t region_offset;
    uint64_t file_offset;
    uint64_t allocation_offset;
    uint64_t length;
} AgfxEeeeSourceInterval;

unsigned agfx_eeee_source_backing_interval_count(const AgfxEeeeSourceBacking *source);
bool agfx_eeee_source_backing_interval(const AgfxEeeeSourceBacking *source,
                                       unsigned index,
                                       AgfxEeeeSourceInterval *interval);
bool agfx_eeee_source_backing_mark_dirty(AgfxEeeeSourceBacking *source,
                                         uint64_t offset, uint64_t length);

/* ------------------------------------------------------------ dma-buf */
typedef struct AgfxEeeeSourceDmaBuf AgfxEeeeSourceDmaBuf;

typedef enum AgfxEeeeDmaBufFailureStage {
    AGFX_DMABUF_NONE,
    AGFX_DMABUF_ALLOCATION,
    AGFX_DMABUF_SOURCE_LAYOUT,
    AGFX_DMABUF_FILE_STAT,
    AGFX_DMABUF_FILE_EXTENT,
    AGFX_DMABUF_FILE_SEALS,
    AGFX_DMABUF_SEAL_POLICY,
    AGFX_DMABUF_CONTROL_OPEN,
    AGFX_DMABUF_CREATE_LIST,
} AgfxEeeeDmaBufFailureStage;

typedef struct AgfxEeeeDmaBufFailure {
    AgfxEeeeDmaBufFailureStage stage;
    int error_no;        /* actual errno, 0 for construction-only rejections */
    unsigned interval;   /* UINT_MAX when no interval is selected */
} AgfxEeeeDmaBufFailure;

/* One /dev/udmabuf open and one exact CREATE_LIST over the saved pieces; the
 * export holds its own source reference. No retry or alternative path. */
AgfxEeeeSourceDmaBuf *agfx_eeee_source_dmabuf_create(
    AgfxEeeeSourceBacking *source, AgfxEeeeDmaBufFailure *failure);
void agfx_eeee_source_dmabuf_ref(AgfxEeeeSourceDmaBuf *export);
void agfx_eeee_source_dmabuf_unref(AgfxEeeeSourceDmaBuf *export);
int agfx_eeee_source_dmabuf_fd(const AgfxEeeeSourceDmaBuf *export);

#endif /* APPLE_PVG_PROCESS_VM_H */
