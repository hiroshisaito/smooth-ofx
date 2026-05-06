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

#ifdef __cplusplus
}
#endif

#endif /* SMOOTH_GPU_FFI_H_ */
