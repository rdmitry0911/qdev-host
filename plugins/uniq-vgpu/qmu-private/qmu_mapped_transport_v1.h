/* Private matched-peer provider for the compatibility child-FIFO/stamp
 * transport.  Optional: an absent export (older DSO) or a session that never
 * installs it keeps the existing read/write-memory callback path unchanged.
 * It changes neither qmu_extended_callbacks nor any embedded handoff layout.
 *
 * Contract RETAINED_RAM_V1: map_retained returns one direct RAM alias for the
 * whole range, captured while no address-space publication overlapped it, and
 * holds that region's lifetime until callbacks.unmap_gpa.  An accepted
 * operation completes on that captured backing even if the GPA is republished
 * later; topology_sequence only withdraws future lookup.  An odd sequence is a
 * publication in progress.  Both functions are callable from any thread.
 * callbacks.mark_mapped_memory_dirty on an HVA inside a live retained capture
 * must succeed; QMetal treats a failure as a fatal contract violation. */
#ifndef QMU_PRIVATE_MAPPED_TRANSPORT_V1_H
#define QMU_PRIVATE_MAPPED_TRANSPORT_V1_H
#include "qmu/qmetal_unified.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QMU_PRIVATE_MAPPED_TRANSPORT_V1_VERSION 1u
#define QMU_MAPPED_TRANSPORT_RETAINED_RAM_V1 1u
struct qmu_mapped_transport_provider_v1 {
    uint32_t version, struct_size;
    uint32_t contract, reserved;
    /* Must equal the session's callbacks.user_ctx. */
    void *ctx;
    void *(*map_retained)(void *ctx, uint64_t gpa, size_t size, int writable,
                          uint64_t *sequence);
    uint64_t (*topology_sequence)(void *ctx);
};
enum qmu_mapped_transport_install_result_v1 {
    QMU_MAPPED_TRANSPORT_INSTALLED = 1,
    QMU_MAPPED_TRANSPORT_REJECTED = 2
};
struct qmu_private_mapped_transport_v1 {
    uint32_t version, struct_size;
    /* Only before the first FIFO enable/child DEFINE; copies the descriptor.
     * REJECTED leaves the session on the callback path, unchanged. */
    uint32_t (*install)(qmu_session *,
                        const struct qmu_mapped_transport_provider_v1 *);
};
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const struct qmu_private_mapped_transport_v1 *
qmu_get_private_mapped_transport_v1(void);
#ifdef __cplusplus
}
#endif
#endif
