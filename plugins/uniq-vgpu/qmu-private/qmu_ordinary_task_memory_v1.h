/* Private matched-peer CPU facilities for the actual ordinary task factory.
 * This installs no canonical SourceSession/Task/Memory and changes neither
 * the public full18 record nor the independent ordinary DMA capture V2. */
#ifndef QMU_PRIVATE_ORDINARY_TASK_MEMORY_V1_H
#define QMU_PRIVATE_ORDINARY_TASK_MEMORY_V1_H
#include "qmu/qmetal_unified.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QMU_PRIVATE_ORDINARY_TASK_MEMORY_V1_VERSION 1u
enum qmu_ordinary_task_memory_profile_v1 {
    /* Explicit descriptor recipe: nil memoryMapDescriptor, real complete
     * legacy create/destroy/map/unmap/read facilities. Not a hardware claim. */
    QMU_ORDINARY_TASK_MEMORY_LEGACY_CALLBACKS = 1
};
struct qmu_ordinary_task_memory_handoff_v1 {
    uint32_t version, struct_size;
    uint32_t profile, reserved;
    /* One acquired reference; only nonnull prepare consumes this token. */
    void *acquired_context;
    void (*release_context)(void *);
    void (*stop_acquiring)(void *);
    /* Actual callback user_ctx, separate from the acquired context handle. */
    void *memory_ctx_identity;
    qmu_extended_callbacks facilities;
};
enum qmu_ordinary_task_memory_install_result_v1 {
    QMU_ORDINARY_TASK_MEMORY_INSTALLED = 1,
    QMU_ORDINARY_TASK_MEMORY_REJECTED = 2,
    QMU_ORDINARY_TASK_MEMORY_ESCAPE_RETAINED = 3,
    QMU_ORDINARY_TASK_MEMORY_UNRETURNED = 4
};
struct qmu_private_ordinary_task_memory_v1 {
    uint32_t version, struct_size;
    void *(*prepare)(qmu_session *,const struct qmu_ordinary_task_memory_handoff_v1 *);
    uint32_t (*install)(void *);
    /* Uninstalled receipts, or receipts whose actual Session destroy leaf
     * returned. False preserves the same owner/escape; never retry/free it.
     * Session destruction remains the existing ordinary V2 owned protocol. */
    bool (*dispose_returned)(void *);
};
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const struct qmu_private_ordinary_task_memory_v1 *
qmu_get_private_ordinary_task_memory_v1(void);
#ifdef __cplusplus
}
#endif
#endif
