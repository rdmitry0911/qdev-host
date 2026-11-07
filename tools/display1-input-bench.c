/* Round-trip latency of org.qemu.Display1.Keyboard Press/Release calls against a
 * Display1 endpoint (QEMU itself, or qdev-host forwarding to QEMU).
 *   display1-input-bench ADDRESS [DESTINATION] [N]
 *   ADDRESS: unix:path=... (p2p when DESTINATION is omitted or "-")
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gio/gio.h>

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s unix:path=... [destination|-] [iterations]\n", argv[0]); return 2; }
    const char *addr = argv[1], *dest = (argc > 2 && strcmp(argv[2], "-")) ? argv[2] : NULL;
    int n = argc > 3 ? atoi(argv[3]) : 2000;
    GError *err = NULL;
    GDBusConnectionFlags fl = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | (dest ? G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION : 0);
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(addr, fl, NULL, NULL, &err);
    if (!c) { fprintf(stderr, "connect: %s\n", err->message); return 1; }
    double *lat = malloc(sizeof(double) * n);
    for (int i = 0; i < n; i++) {
        double t0 = now_us();
        GVariant *r = g_dbus_connection_call_sync(c, dest, "/org/qemu/Display1/Console_0", "org.qemu.Display1.Keyboard",
                                                  (i & 1) ? "Release" : "Press", g_variant_new("(u)", 0x1e /* 'a' */),
                                                  NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
        lat[i] = now_us() - t0;
        if (!r) { fprintf(stderr, "call %d: %s\n", i, err->message); return 1; }
        g_variant_unref(r);
    }
    qsort(lat, n, sizeof(double), cmp);
    double sum = 0; for (int i = 0; i < n; i++) sum += lat[i];
    printf("Keyboard.Press/Release round trip over %d calls: avg %.1f us, median %.1f us, p99 %.1f us, max %.1f us\n",
           n, sum / n, lat[n / 2], lat[(int)(n * 0.99)], lat[n - 1]);
    return 0;
}
