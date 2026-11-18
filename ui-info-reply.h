/*
 * The org.qemu.Display1 Console.SetUIInfo reply for the result of a plugin's
 * qdp v3 ui_info() (qdev-host.c host_ui_info), kept free of GLib so that the
 * mapping is unit-tested natively (tools/display1-uiinfo-test.c).
 *
 * Reference: QEMU ui/dbus-console.c dbus_console_set_ui_info() answers
 * org.qemu.Display1.Error.Unsupported "SetUIInfo is not supported" when the
 * console has no ui_info, and otherwise completes the call; the error domain
 * (ui/dbus-error.c) also has org.qemu.Display1.Error.Invalid and
 * org.qemu.Display1.Error.Failed. A size the device refuses is an error of
 * the call too, so a UI can tell a refused size from an accepted one instead
 * of asking again.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QDEV_UI_INFO_REPLY_H
#define QDEV_UI_INFO_REPLY_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "qdp.h"

#define DISPLAY1_ERROR_FAILED      "org.qemu.Display1.Error.Failed"
#define DISPLAY1_ERROR_INVALID     "org.qemu.Display1.Error.Invalid"
#define DISPLAY1_ERROR_UNSUPPORTED "org.qemu.Display1.Error.Unsupported"

/* NULL: complete SetUIInfo normally (0 accepted, QDP_UI_INFO_UNCHANGED).
 * Otherwise the D-Bus error name to answer with, its message in msg:
 *   -ENOTSUP  Unsupported, QEMU's text: the plugin has no ui_info, or does not
 *             follow UI sizes (display-modes=fixed, a libmlapi without
 *             qmu_display_set_mode_list);
 *   -EINVAL   Invalid "<W>x<H> refused: ...": outside the device's limits (the
 *             plugin logs which one; the qdp ABI carries no reason);
 *   other     Failed, the errno text (-ENODEV stopped, -EIO, ...). */
static inline const char *ui_info_dbus_error(int r, unsigned w, unsigned h, char *msg, size_t len)
{
    if (r == 0 || r == QDP_UI_INFO_UNCHANGED) {
        if (len) {
            msg[0] = 0;
        }
        return NULL;
    }
    if (r == -ENOTSUP) {
        snprintf(msg, len, "SetUIInfo is not supported");
        return DISPLAY1_ERROR_UNSUPPORTED;
    }
    if (r == -EINVAL) {
        snprintf(msg, len, "%ux%u refused: outside the device's mode limits (see the device log)", w, h);
        return DISPLAY1_ERROR_INVALID;
    }
    if (r < 0) {
        snprintf(msg, len, "%ux%u: %s", w, h, strerror(-r));
    } else {
        snprintf(msg, len, "%ux%u: unexpected ui_info result %d", w, h, r);
    }
    return DISPLAY1_ERROR_FAILED;
}

#endif
