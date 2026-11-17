/*
 * test_plugin.c - smoke test for the apple-videotoolbox qdp plugin .so:
 * dlopen it, resolve qdp_plugin_v1, and verify it presents a well-formed
 * device-ops table with the expected identity. Does NOT call setup() (that
 * needs the libvfio-user host); this checks the plugin loads and exports the
 * ABI the host expects. CLEAN-ROOM. Increment 1, lane L-M2V03, 2026-11-16.
 */
#define _GNU_SOURCE
#include "qdp.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *so = (argc > 1) ? argv[1] : "apple-videotoolbox.so";
    void *h;
    qdp_plugin_v1_fn entry;
    const qdp_device_ops *ops;
    int fail = 0;

    printf("apple-videotoolbox: plugin ABI smoke test (%s)\n", so);

    h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        /* RTLD_NOW fails because vfu_* are host-resolved; retry lazy so we can
         * still check the exported table without the host present. */
        h = dlopen(so, RTLD_LAZY | RTLD_LOCAL);
    }
    if (!h) { printf("FAIL: dlopen: %s\n", dlerror()); return 1; }

    entry = (qdp_plugin_v1_fn)dlsym(h, QDP_PLUGIN_SYMBOL);
    if (!entry) { printf("FAIL: no %s\n", QDP_PLUGIN_SYMBOL); return 1; }

    ops = entry();
    if (!ops) { printf("FAIL: qdp_plugin_v1 returned NULL\n"); return 1; }

#define CK(c, m) do { if (!(c)) { fail++; printf("  FAIL: %s\n", m); } } while (0)
    CK(ops->abi_version == QDP_ABI_VERSION, "abi_version");
    CK(ops->name && !strcmp(ops->name, "apple-videotoolbox"), "name");
    CK(ops->version != NULL, "version string");
    CK(ops->setup != NULL, "setup present");
    CK(ops->teardown != NULL, "teardown present");
    CK(ops->reset != NULL, "reset present");
#undef CK

    printf("  device: name=%s version=%s abi=%u\n",
           ops->name, ops->version, ops->abi_version);
    printf("%s: plugin ABI smoke test\n", fail ? "FAIL" : "PASS");
    dlclose(h);
    return fail ? 1 : 0;
}
