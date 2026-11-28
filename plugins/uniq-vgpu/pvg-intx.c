/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pvg-intx: level-triggered INTx state of the uniq-vgpu device (see pvg-intx.h). */
#include "pvg-intx.h"

#include <errno.h>
#include <string.h>
#include "pci_caps/msi.h"

void pvg_intx_reset(PvgIntx *x)
{
    memset(x, 0, sizeof(*x));
}

static bool pvg_intx_assert(PvgIntx *x)
{
    x->asserted = true;
    x->masked = true;                      /* automask until the client's UNMASK */
    x->signalled_seq = x->raise_seq;
    x->asserts++;
    return true;
}

bool pvg_intx_raise(PvgIntx *x)
{
    x->raise_seq++;
    if (!x->masked && !x->asserted) {
        return pvg_intx_assert(x);
    }
    x->held++;                             /* outstanding until the next UNMASK decides */
    return false;
}

void pvg_intx_ack(PvgIntx *x, enum pvg_intx_ack_src src)
{
    if ((unsigned)src >= PVG_INTX_ACK_COUNT) {
        return;
    }
    x->ack_seq[src] = x->raise_seq;
    x->acked[src] = true;
}

int pvg_intx_ack_source(uint32_t bar0_offset)
{
    switch (bar0_offset) {
    case PVG_INTX_ACK_DISPLAY_IRQ:
    case PVG_INTX_ACK_EVENT_STAMPS:
        return PVG_INTX_ACK_GPU;
    case PVG_INTX_ACK_RING_RD:
        return PVG_INTX_ACK_MAPPER;
    default:
        return -1;
    }
}

/* Every source that has ever acknowledged did so after the last raise (and at least one has). */
static bool pvg_intx_all_acked(const PvgIntx *x)
{
    bool any = false;

    for (int i = 0; i < PVG_INTX_ACK_COUNT; i++) {
        if (!x->acked[i]) {
            continue;
        }
        any = true;
        if (x->ack_seq[i] < x->raise_seq) {
            return false;
        }
    }
    return any;
}

void pvg_intx_cancel_assert(PvgIntx *x)
{
    if (x->asserted) {
        x->asserted = false;
        x->masked = false;
        x->asserts--;
    }
}

bool pvg_intx_set_mask(PvgIntx *x, bool mask)
{
    if (mask) {
        x->masked = true;
        return false;
    }
    /* UNMASK: the client has deasserted the line (EOI) and unmasked the source. */
    x->masked = false;
    x->asserted = false;
    if (x->raise_seq == x->signalled_seq) {
        return false;                      /* no raise since the last assertion */
    }
    if (pvg_intx_all_acked(x)) {
        x->signalled_seq = x->raise_seq;   /* the handlers read every cause: stay deasserted */
        x->suppressed++;
        return false;
    }
    x->reasserts++;
    return pvg_intx_assert(x);
}

/* ------------------------------------------------------------------------------------------------------------ */
void pvg_irq_line_init(PvgIrqLine *l, int mode, vfu_ctx_t *ctx, pvg_irq_trigger_fn trigger, void *host)
{
    memset(l, 0, sizeof(*l));
    l->mode = mode;
    l->ctx = ctx;
    l->trigger = trigger;
    l->host = host;
    l->index = -1;
}

int pvg_irq_line_setup(PvgIrqLine *l, vfu_dev_irq_state_cb_t *cb)
{
    struct msicap msi = {
        .hdr = { .id = PCI_CAP_ID_MSI },
        .mc = { .c64 = 1 },      /* 1 vector (mmc 0), 64-bit, no per-vector mask */
    };

    if (l->mode != PVG_IRQ_MODE_INTX &&
        (vfu_pci_add_capability(l->ctx, 0, 0, &msi) < 0 || vfu_setup_device_nr_irqs(l->ctx, VFU_DEV_MSI_IRQ, 1) < 0)) {
        return -errno;
    }
    if (l->mode != PVG_IRQ_MODE_MSI &&
        (vfu_setup_device_nr_irqs(l->ctx, VFU_DEV_INTX_IRQ, 1) < 0 ||          /* INTA# in config space */
         vfu_setup_irq_state_callback(l->ctx, VFU_DEV_INTX_IRQ, cb) < 0)) {
        return -errno;
    }
    return 0;
}

bool pvg_irq_line_is_intx(PvgIrqLine *l, bool *changed)
{
    int idx;

    if (changed) {
        *changed = false;
    }
    if (l->mode == PVG_IRQ_MODE_MSI || !l->ctx) {
        return false;
    }
    idx = vfu_irq_trigger_index(l->ctx);
    if (idx != l->index) {
        if (idx == VFU_DEV_INTX_IRQ) {
            pvg_intx_reset(&l->intx);   /* the client enabled INTx: line deasserted, unmasked */
        }
        l->index = idx;
        if (changed) {
            *changed = true;
        }
    }
    return idx == VFU_DEV_INTX_IRQ;
}

int pvg_irq_line_raise(PvgIrqLine *l, uint32_t vector, bool *intx)
{
    bool is_intx = pvg_irq_line_is_intx(l, NULL);

    if (intx) {
        *intx = is_intx;
    }
    if (is_intx) {
        return pvg_intx_raise(&l->intx) ? l->trigger(l->host, 0) : 0;
    }
    return l->trigger(l->host, vector);
}

void pvg_irq_line_bar0_read(PvgIrqLine *l, uint32_t offset)
{
    int src;

    if (l->mode == PVG_IRQ_MODE_MSI || (src = pvg_intx_ack_source(offset)) < 0) {
        return;
    }
    pvg_intx_ack(&l->intx, (enum pvg_intx_ack_src)src);
}

int pvg_irq_line_mask(PvgIrqLine *l, bool mask, bool can_signal, bool *deferred)
{
    if (deferred) {
        *deferred = false;
    }
    if (!pvg_irq_line_is_intx(l, NULL) || !pvg_intx_set_mask(&l->intx, mask)) {
        return 0;
    }
    if (can_signal) {
        return l->trigger(l->host, 0);
    }
    pvg_intx_cancel_assert(&l->intx);
    if (deferred) {
        *deferred = true;
    }
    return 0;
}

void pvg_irq_line_reset(PvgIrqLine *l)
{
    pvg_intx_reset(&l->intx);
    l->index = -1;
}
