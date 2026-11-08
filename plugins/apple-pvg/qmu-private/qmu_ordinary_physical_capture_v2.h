/* Complete PRIVATE DMA matched-peer protocol. Public qmu_config/full18 unchanged. */
#ifndef QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_H
#define QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct qmu_session qmu_session;
#define QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_VERSION 2u
struct qmu_ordinary_physical_piece_v2 {
    uint64_t gpa, logical_offset, length;
    /* Original map RETURN witness, never an address to reverse-translate. */
    const void *mapped_pointer;
};
struct qmu_ordinary_physical_request_v2 {
    uint32_t version, struct_size;
    uint32_t task_id, resource_id;
    uint64_t task_epoch, object_epoch, source_va;
    /* Every byte of allocation_length is covered by captured pieces. */
    uint64_t allocation_length, visible_offset, literal_length, allocation_alignment;
    uint64_t piece_count;
    const struct qmu_ordinary_physical_piece_v2 *pieces;
};
struct qmu_ordinary_physical_view_v2 {
    void *allocation;
    uint64_t allocation_length;
    void *visible_pointer;
    uint64_t visible_offset, visible_length;
    /* Borrowed real export owned by SAME span until explicit release_span.
     * fd0 is valid. Describe succeeds only after export returned fd >= 0. */
    int32_t dma_buf_fd;
    uint32_t reserved; /* zero */
};
struct qmu_ordinary_physical_handoff_v2 {
    uint32_t version, struct_size;
    /* One genuinely acquired reference, consumed only by nonnull prepare. */
    void *acquired_context;
    void *memory_ctx_identity; /* exact ordinary callbacks.user_ctx */
    bool (*stop)(void *);
    void (*release_context)(void *);
    /* V2 selects DMA export before native import, with no HOST fallback.
     * Publish partial owner BEFORE first MR/FD/mapping/export effect. A false
     * describe leaves that SAME owner for explicit returned retirement. */
    void (*capture)(void *, const struct qmu_ordinary_physical_request_v2 *, void **);
    bool (*describe)(const void *, struct qmu_ordinary_physical_view_v2 *);
    bool (*mark_dirty)(void *, uint64_t, uint64_t);
    void (*release_span)(void *);
};
enum qmu_ordinary_physical_install_result_v2 {
    QMU_ORDINARY_PHYSICAL_V2_INSTALLED = 1,
    QMU_ORDINARY_PHYSICAL_V2_REJECTED = 2,
    QMU_ORDINARY_PHYSICAL_V2_ESCAPE_RETAINED = 3,
    QMU_ORDINARY_PHYSICAL_V2_UNRETURNED = 4
};
struct qmu_private_ordinary_physical_capture_v2 {
    uint32_t version, struct_size;
    void *(*prepare)(qmu_session *, const struct qmu_ordinary_physical_handoff_v2 *);
    uint32_t (*install)(void *);
    /* False preserves original receipt/context/exception. No retry. */
    bool (*dispose_returned)(void *);
    /* Synchronous owned-worker call: true requires actual final delete leaf
     * AND normal qmu_destroy RETURN. Null receipt covers prepare rejection;
     * negotiate this complete getter before creating a Session. False retains
     * Session/receipt/context; it is never permission to retry or free them. */
    bool (*destroy_returned)(void *, qmu_session *);
};
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const struct qmu_private_ordinary_physical_capture_v2 *
qmu_get_private_ordinary_physical_capture_v2(void);
#ifdef __cplusplus
}
#endif
#endif
