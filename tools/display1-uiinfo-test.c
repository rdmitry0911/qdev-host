/* org.qemu.Display1 Console.SetUIInfo reply of qdev-host (ui-info-reply.h).
 *   display1-uiinfo-test                       unit cases of ui_info_dbus_error()
 *   display1-uiinfo-test ADDRESS W H EXPECT [MESSAGE]
 *       one SetUIInfo(0, 0, 0, 0, W, H) call on the p2p ADDRESS (unix:path=...)
 *       of a running qdev-host; EXPECT is "ok" (the call completes) or the
 *       D-Bus error name it must answer with, MESSAGE the expected text.
 * Reference: QEMU ui/dbus-console.c dbus_console_set_ui_info() (Unsupported,
 * "SetUIInfo is not supported", for a console without ui_info) and the
 * ui/dbus-error.c names Failed/Invalid/Unsupported.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include "ui-info-reply.h"

static int checks, failures;

static void check(int ok, const char *what, const char *got_name, const char *got_msg)
{
    checks++;
    failures += !ok;
    printf("%s %s -> %s \"%s\"\n", ok ? "PASS" : "FAIL", what, got_name ? got_name : "(complete)", got_msg);
}

static void unit_case(int r, unsigned w, unsigned h, const char *name, const char *msg, const char *what)
{
    char got[160];
    const char *e = ui_info_dbus_error(r, w, h, got, sizeof(got));
    int ok = name ? e && !strcmp(e, name) : !e;

    ok = ok && !strcmp(got, msg);
    check(ok, what, e, got);
}

static int unit(void)
{
    unit_case(0, 1630, 572, NULL, "", "0 (accepted)");
    unit_case(QDP_UI_INFO_UNCHANGED, 1630, 572, NULL, "", "QDP_UI_INFO_UNCHANGED");
    unit_case(-ENOTSUP, 1630, 572, DISPLAY1_ERROR_UNSUPPORTED, "SetUIInfo is not supported",
              "-ENOTSUP (no ui_info, display-modes=fixed, no qmu_display_set_mode_list): QEMU's reply");
    unit_case(-EINVAL, 65536, 1000, DISPLAY1_ERROR_INVALID,
              "65536x1000 refused: outside the device's mode limits (see the device log)", "-EINVAL");
    unit_case(-ENODEV, 1600, 900, DISPLAY1_ERROR_FAILED, "1600x900: No such device", "-ENODEV (device stopped)");
    unit_case(-EIO, 1600, 900, DISPLAY1_ERROR_FAILED, "1600x900: Input/output error",
              "-EIO (qmu_display_set_mode_list failed)");
    unit_case(7, 1600, 900, DISPLAY1_ERROR_FAILED, "1600x900: unexpected ui_info result 7",
              "an undefined positive result");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static int live(const char *addr, unsigned w, unsigned h, const char *expect, const char *want_msg)
{
    GError *err = NULL;
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                                NULL, NULL, &err);
    char what[96];

    if (!c) {
        fprintf(stderr, "connect %s: %s\n", addr, err->message);
        return 1;
    }
    GVariant *r = g_dbus_connection_call_sync(c, NULL, "/org/qemu/Display1/Console_0", "org.qemu.Display1.Console",
                                              "SetUIInfo", g_variant_new("(qqiiuu)", 0, 0, 0, 0, w, h), NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    snprintf(what, sizeof(what), "SetUIInfo %ux%u, want %s", w, h, expect);
    if (r) {
        g_variant_unref(r);
        check(!strcmp(expect, "ok"), what, NULL, "");
    } else {
        g_autofree char *name = g_dbus_error_get_remote_error(err);

        g_dbus_error_strip_remote_error(err);
        check(name && !strcmp(name, expect) && (!want_msg || !strcmp(err->message, want_msg)), what,
              name ? name : "(local)", err->message);
        g_error_free(err);
    }
    g_object_unref(c);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        return unit();
    }
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s [ADDRESS W H ok|ERROR-NAME [MESSAGE]]\n", argv[0]);
        return 2;
    }
    return live(argv[1], (unsigned)strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10), argv[4],
                argc == 6 ? argv[5] : NULL);
}
