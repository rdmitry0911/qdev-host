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
/* new scanout (enabled with pixels/size) or blank (enabled == false) */
void kms_scanout(kms_t *k, const uint8_t *pix, uint32_t w, uint32_t h, uint32_t stride, bool enabled);
/* damage in scanout coordinates; pix is the current scanout base pointer */
void kms_update(kms_t *k, const uint8_t *pix, uint32_t stride, int x, int y, int w, int h);
void kms_stats(kms_t *k, FILE *out, bool reset);
/* "connected"/"disconnected"/"unknown" from sysfs for the chosen connector */
const char *kms_connector_status(kms_t *k);
#endif
