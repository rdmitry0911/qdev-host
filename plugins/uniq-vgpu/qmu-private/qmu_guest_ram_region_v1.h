/* Private matched-peer provider for guest-RAM DMA region bounds (ML-SGR-2).
 * Optional: an absent export (older DSO) or a session that never installs it
 * keeps every QMetal consumer that needs bounds off (the ordinary Shared chunk
 * cache stays disabled).  It changes neither qmu_extended_callbacks nor any
 * embedded handoff layout.
 *
 * region(ctx, gpa, &base, &length) returns 1 when gpa lies in a live DMA
 * region that callbacks.map_gpa can map (it has a host mapping), and then
 * [base, base + length) is that region's whole guest-physical range; it
 * returns 0 otherwise and leaves the outputs untouched.  It takes no pin,
 * maps nothing and never logs.  The answer is a snapshot: a later map_gpa of
 * a range inside it is still checked by the host, so a topology change in
 * between is not a contract violation.  Callable from any thread. */
#ifndef QMU_PRIVATE_GUEST_RAM_REGION_V1_H
#define QMU_PRIVATE_GUEST_RAM_REGION_V1_H
#include "qmu/qmetal_unified.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QMU_PRIVATE_GUEST_RAM_REGION_V1_VERSION 1u
struct qmu_guest_ram_region_provider_v1 {
    uint32_t version, struct_size;
    /* Must equal the session's callbacks.user_ctx. */
    void *ctx;
    int (*region)(void *ctx, uint64_t gpa, uint64_t *base, uint64_t *length);
};
enum qmu_guest_ram_region_install_result_v1 {
    QMU_GUEST_RAM_REGION_INSTALLED = 1,
    QMU_GUEST_RAM_REGION_REJECTED = 2
};
struct qmu_private_guest_ram_region_v1 {
    uint32_t version, struct_size;
    /* At most once per session; copies the descriptor.  REJECTED leaves the
     * session without bounds, unchanged. */
    uint32_t (*install)(qmu_session *,
                        const struct qmu_guest_ram_region_provider_v1 *);
};
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const struct qmu_private_guest_ram_region_v1 *
qmu_get_private_guest_ram_region_v1(void);
#ifdef __cplusplus
}
#endif
#endif
