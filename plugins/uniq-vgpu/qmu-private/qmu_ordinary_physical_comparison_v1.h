/* Optional PRIVATE comparison over retained V2 spans. V2/full18 unchanged. */
#ifndef QMU_PRIVATE_ORDINARY_PHYSICAL_COMPARISON_V1_H
#define QMU_PRIVATE_ORDINARY_PHYSICAL_COMPARISON_V1_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct qmu_session qmu_session;
#define QMU_PRIVATE_ORDINARY_PHYSICAL_COMPARISON_V1_VERSION 1u
enum qmu_ordinary_physical_comparison_result_v1 {
    QMU_ORDINARY_PHYSICAL_COMPARISON_UNAVAILABLE = 0,
    QMU_ORDINARY_PHYSICAL_COMPARISON_DISJOINT = 1,
    QMU_ORDINARY_PHYSICAL_COMPARISON_OVERLAP = 2
};
struct qmu_ordinary_physical_comparison_handoff_v1 {
    uint32_t version, struct_size;
    void *memory_ctx_identity; /* exact installed V2 callbacks.user_ctx */
    void *physical_context_identity; /* exact installed V2 acquired_context */
    /* Borrow both actual retained spans synchronously. Offsets/lengths refer
     * to V2 visible_pointer/visible_length, excluding allocation padding.
     * No guest mapping, BQL, native effects or ownership transfer. */
    uint32_t (*compare)(const void *physical_context_identity,
        const void *left_span, uint64_t left_offset, uint64_t left_length,
        const void *right_span, uint64_t right_offset, uint64_t right_length);
};
struct qmu_private_ordinary_physical_comparison_v1 {
    uint32_t version, struct_size;
    /* Install once before ingress against SAME installed V2 provider.
     * false consumes nothing; absence leaves comparison UNAVAILABLE. */
    bool (*install)(qmu_session *,
        const struct qmu_ordinary_physical_comparison_handoff_v1 *);
};
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const struct qmu_private_ordinary_physical_comparison_v1 *
qmu_get_private_ordinary_physical_comparison_v1(void);
#ifdef __cplusplus
}
#endif
#endif
