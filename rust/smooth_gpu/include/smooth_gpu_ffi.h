// smooth_gpu FFI surface, exposed by rust/smooth_gpu/src/lib.rs.
//
// Header is hand-maintained (no cbindgen) to keep the build deterministic.
// Bump smooth_gpu_version() lower 16 bits whenever any struct here changes
// shape; the OFX C++ side reads that probe at link time to refuse mismatched
// staticlib/header pairings.
//
// CALLER CONTRACT
// ---------------
//
//   Initialisation
//     smooth_gpu_init() must succeed (return SMOOTH_GPU_STATUS_OK = 0)
//     before any data-plane entry point is called. A non-zero return means
//     the GPU is unavailable on this host; the C++ side should fall back to
//     the CPU `smooth_core_*` path. The init is process-wide and idempotent.
//
//   Buffer aliasing
//     `src` and `dst` for the data-plane entry points must NOT overlap. We
//     read src once into a GPU-local buffer, dispatch, and write dst from a
//     GPU readback. Passing the same pointer is undefined.
//
//   Threading
//     Entry points are thread-safe; an internal mutex serialises GPU
//     submissions today. A future multi-stream pipeline may relax this.
//
//   Cross-platform
//     macOS:   Metal backend, no extra OS prereqs beyond a working GPU.
//     Windows: DX12 (or Vulkan if the system has it). Provided by the OS.
//     Linux:   Vulkan via system ICD. The OFX bundle does NOT ship a Vulkan
//              loader; install `mesa-vulkan-drivers` (or vendor equivalent)
//              before loading the plugin.

#ifndef SMOOTH_GPU_FFI_H_
#define SMOOTH_GPU_FFI_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Linkage probe. Upper 16 bits = major (0 = pre-release scaffolding),
 * lower 16 bits = FFI revision. */
uint32_t smooth_gpu_version(void);

/* Build identity: "<crate-semver>+<tag-or-git-sha>[+dirty]". Returns a
 * pointer to a static null-terminated ASCII string valid for the process
 * lifetime; do NOT free it. */
const char *smooth_gpu_build_id(void);

/* Status codes returned by every entry point below. */
#define SMOOTH_GPU_STATUS_OK                  0u
#define SMOOTH_GPU_STATUS_NO_ADAPTER          1u
#define SMOOTH_GPU_STATUS_DEVICE_CREATE_FAIL  2u
#define SMOOTH_GPU_STATUS_PIPELINE_FAIL       3u
#define SMOOTH_GPU_STATUS_DISPATCH_FAIL       4u
#define SMOOTH_GPU_STATUS_NULL_POINTER        5u

/* Lazy GPU init. Returns SMOOTH_GPU_STATUS_OK if a usable adapter + device
 * is available; otherwise SMOOTH_GPU_STATUS_NO_ADAPTER /
 * SMOOTH_GPU_STATUS_DEVICE_CREATE_FAIL. Idempotent. */
uint32_t smooth_gpu_init(void);

/* Phase B: round-trip `len` u32 elements through a passthrough compute
 * shader. Reads src[0..len], writes dst[0..len]. Pointers must be 4-byte
 * aligned and non-overlapping. Used by the OFX side as a smoke test that
 * the GPU pipeline is alive on the current host. The real algorithm
 * entry points (preprocess + process_row_range) will land in Phase F. */
uint32_t smooth_gpu_passthrough_u32(const uint32_t *src, uint32_t *dst, size_t len);

/* SmoothBbox shape: matches smooth_core's smooth_bbox_t. */
typedef struct {
    int32_t top;
    int32_t left;
    int32_t right;
    int32_t bottom;
} smooth_gpu_bbox_t;

/* Phase F-1: u8 (ARGB) preprocess on GPU.
 *
 * Reads `src_ptr[0..(rowbytes/4) * height]`, writes `out_ptr[0..same]`, and
 * fills `*bbox_out` with the SmoothBbox-shaped result.
 *
 *   if is_white_trans != 0:
 *     - white-key (RGB == 0xFFFFFF) pixels become null (RGBA = 0)
 *     - alpha == 0 pixels are skipped (out is unchanged from src)
 *     - other pixels are passed through and contribute to bbox
 *   else:
 *     - all pixels are passed through unchanged
 *     - bbox spans only non-white, non-zero-alpha pixels
 *
 * `rowbytes` must equal `width * 4` — tight buffers only for now (matches
 * how smooth_core_preprocess_u8 is currently called from the OFX C++ side).
 *
 * src_ptr and out_ptr may alias (same pointer is fine; we always stage src
 * to a GPU buffer and write out_ptr from the GPU readback). */
uint32_t smooth_gpu_preprocess_u8(
    const uint32_t   *src_ptr,
    uint32_t         *out_ptr,
    int32_t           rowbytes,
    int32_t           height,
    int32_t           is_white_trans,
    smooth_gpu_bbox_t *bbox_out);

#ifdef __cplusplus
}
#endif

#endif /* SMOOTH_GPU_FFI_H_ */
