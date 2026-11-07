/* DRM/KMS sink: scan the device framebuffer out on a physical connector.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KMS_SINK_H
#define KMS_SINK_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct kms kms_t;

/* spec: "/dev/dri/card0" (first connected connector) or "/dev/dri/card0:eDP-1" */
kms_t *kms_open(const char *spec, bool verbose);
void   kms_close(kms_t *k);
const uint8_t *kms_edid(kms_t *k, uint32_t *len);
const char *kms_connector_name(kms_t *k);
/* new scanout (enabled with pixels/size) or blank (enabled == false).
 * memfd/offset describe the same pixels as a file; with them (and a mode of the
 * exact size) the sink scans the memfd out directly (udmabuf, zero-copy). */
void kms_scanout(kms_t *k, const uint8_t *pix, uint32_t w, uint32_t h, uint32_t stride, bool enabled,
                 int memfd, uint32_t offset);
bool kms_zero_copy(kms_t *k);
void kms_set_allow_zero_copy(kms_t *k, bool allow);
/* damage in scanout coordinates; pix is the current scanout base pointer */
void kms_update(kms_t *k, const uint8_t *pix, uint32_t stride, int x, int y, int w, int h);
void kms_stats(kms_t *k, FILE *out, bool reset);
/* write the buffer currently scanned out as PPM (what the monitor shows) */
bool kms_dump_front(kms_t *k, const char *path);
/* "connected"/"disconnected"/"unknown" from sysfs for the chosen connector */
const char *kms_connector_status(kms_t *k);
#endif
