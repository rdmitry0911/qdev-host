/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pvg-intx: the device side of a level-triggered PCI INTx line over vfio-user (D-ARM64-INTX).
 *
 * The uniq-vgpu device raises one interrupt for the GPU and the IOSurface mapper (QMetal's raise_irq, vector 0;
 * on arm64 the UniqVGPUArmBridge kext fans it out to both stock drivers).  Under MSI each raise is one message.
 * Under INTx the vfio-user client owns the line, the way QEMU's hw/vfio/pci.c does for every vfio device:
 *   - the device signals its INTx eventfd = ASSERT;
 *   - the client raises the guest line and masks the source (automask); a raise while asserted or masked is
 *     not a new edge;
 *   - at the guest's EOI the client deasserts the line and sends UNMASK (QEMU's userspace path:
 *     vfio_pci_intx_eoi() on the first BAR access after the assertion).
 * A level device re-asserts after UNMASK while its condition is still pending.  Here the condition is "a raise
 * the guest's interrupt handler has not acknowledged".  The acknowledgement is the read the stock handlers do,
 * the same registers the MSI path relies on: the GPU reads the read-and-clear status registers 0x1014
 * (DISPLAY_IRQ) and 0x1018 (EVENT_STAMPS); the mapper reads its ring read counter 0x1250 (RING_RD).  An ack is
 * recorded before the read is served, so it covers exactly the raises whose causes that read can return.
 *
 * Raises are not attributed to a source (QMetal raises vector 0 for both), so after UNMASK the line stays
 * deasserted only if every source that has ever acknowledged has acknowledged after the last raise; otherwise it
 * re-asserts.  A re-assert needs a raise after the previous assertion: an unanswered line never re-fires by
 * itself (no storm), and no raise is left without an interrupt after it (no loss).
 *
 * Pure state, no locking: the caller serializes every call (uniq-vgpu: irq_lock).
 */
#ifndef PVG_INTX_H
#define PVG_INTX_H

#include <stdbool.h>
#include <stdint.h>
#include "libvfio-user.h"

enum pvg_intx_ack_src { PVG_INTX_ACK_GPU = 0, PVG_INTX_ACK_MAPPER = 1, PVG_INTX_ACK_COUNT };

/* BAR0 offsets whose guest read is a handler acknowledgement */
#define PVG_INTX_ACK_DISPLAY_IRQ   0x1014u   /* PVG_REG_DISPLAY_IRQ, read-and-clear */
#define PVG_INTX_ACK_EVENT_STAMPS  0x1018u   /* PVG_REG_EVENT_STAMPS, read-and-clear */
#define PVG_INTX_ACK_RING_RD       0x1250u   /* IOSurface mapper REG_RING_RD (low dword) */

typedef struct PvgIntx {
    bool masked;                           /* the client masked the source (MASK), or automask after ASSERT */
    bool asserted;                         /* ASSERT signalled and no UNMASK since */
    uint64_t raise_seq;                    /* raises so far */
    uint64_t signalled_seq;                /* raise_seq at the last ASSERT */
    uint64_t ack_seq[PVG_INTX_ACK_COUNT];  /* raise_seq when that source's last ack read started */
    bool acked[PVG_INTX_ACK_COUNT];        /* the source has acknowledged at least once */
    uint64_t asserts, reasserts, suppressed, held; /* counters for the device log */
} PvgIntx;

/* The state the client starts INTx in: line deasserted, source unmasked, no history. */
void pvg_intx_reset(PvgIntx *x);

/* A device raise.  Returns true when the caller must ASSERT now (signal the INTx eventfd). */
bool pvg_intx_raise(PvgIntx *x);

/* The guest is about to read the ack register of src (call before the read is served). */
void pvg_intx_ack(PvgIntx *x, enum pvg_intx_ack_src src);

/* The ack source of a BAR0 read at offset, or -1. */
int pvg_intx_ack_source(uint32_t bar0_offset);

/* The client's MASK (mask = true) or UNMASK.  Returns true when the caller must ASSERT now. */
bool pvg_intx_set_mask(PvgIntx *x, bool mask);

/* The caller could not signal the ASSERT pvg_intx_set_mask() asked for (device interrupts held): the line stays
 * down and unmasked, so the caller's next pvg_intx_raise() asserts. */
void pvg_intx_cancel_assert(PvgIntx *x);

/* ------------------------------------------------------------------------------------------------------------
 * The device's interrupt line over libvfio-user (uniq-vgpu irq= msi|intx|auto), shared by the plugin and its
 * native test.  The caller serializes every call (uniq-vgpu: irq_lock).
 */
enum { PVG_IRQ_MODE_MSI, PVG_IRQ_MODE_INTX, PVG_IRQ_MODE_AUTO };

typedef int (*pvg_irq_trigger_fn)(void *host, uint32_t subindex);   /* qdp_host_ops.irq_trigger */

typedef struct PvgIrqLine {
    int mode;                  /* PVG_IRQ_MODE_* */
    vfu_ctx_t *ctx;            /* for vfu_irq_trigger_index() */
    pvg_irq_trigger_fn trigger;
    void *host;
    int index;                 /* the client's trigger index last seen (-1: none) */
    PvgIntx intx;              /* INTx level state while index == VFU_DEV_INTX_IRQ */
} PvgIrqLine;

/* mode, the context and the trigger; index -1, INTx state reset. */
void pvg_irq_line_init(PvgIrqLine *l, int mode, vfu_ctx_t *ctx, pvg_irq_trigger_fn trigger, void *host);

/* PCI interrupt resources for the mode: the MSI capability (+ 1 MSI vector) unless intx, INTA# with the
 * IRQ-state callback cb unless msi.  Returns 0 or -errno. */
int pvg_irq_line_setup(PvgIrqLine *l, vfu_dev_irq_state_cb_t *cb);

/* Re-read the client's trigger index; *changed (optional) = the index changed.  Returns true for INTx. */
bool pvg_irq_line_is_intx(PvgIrqLine *l, bool *changed);

/* One device raise.  MSI/MSI-X: one trigger.  INTx: through the level state.  *intx = the INTx path was used.
 * Returns the trigger's result, or 0 when the INTx raise waits for the client's UNMASK. */
int pvg_irq_line_raise(PvgIrqLine *l, uint32_t vector, bool *intx);

/* A guest BAR0 read at offset is about to be served (INTx acknowledgement). */
void pvg_irq_line_bar0_read(PvgIrqLine *l, uint32_t offset);

/* The client's INTx MASK/UNMASK.  can_signal: the device may trigger now; otherwise a re-assert is returned in
 * *deferred and the caller raises it again later.  Returns the trigger's result or 0. */
int pvg_irq_line_mask(PvgIrqLine *l, bool mask, bool can_signal, bool *deferred);

/* Device reset or a new client: INTx starts again from the client's enable. */
void pvg_irq_line_reset(PvgIrqLine *l);

#endif
