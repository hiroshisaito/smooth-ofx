// smooth_gpu — GPU acceleration for the smooth OFX plugin via wgpu.
//
// Phase A scaffold: minimal FFI surface + version probe. No real kernels yet.
// The C++ side calls smooth_gpu_version() / smooth_gpu_init() to confirm
// linkage and (later) probe whether a usable GPU was found at runtime.
//
// Cross-platform notes:
// - macOS:   wgpu Metal backend. No additional system libs needed.
// - Windows: wgpu DX12 backend (or Vulkan if available). Links to system
//            d3d12.lib / dxgi.lib at the OS level via wgpu's runtime loader,
//            so static lib stays free of import deps.
// - Linux:   wgpu Vulkan backend. The OFX bundle does NOT bundle Vulkan;
//            the host system must have a Vulkan driver installed
//            (mesa-vulkan-drivers or equivalent). Document this in BUILDING
//            when the Linux path is exercised.

use std::sync::Once;

/// Linkage / ABI probe.
/// Upper 16 bits = major (0 = pre-release scaffolding), lower 16 bits = FFI
/// revision. Bump the lower 16 bits whenever any FFI struct changes layout.
#[no_mangle]
pub extern "C" fn smooth_gpu_version() -> u32 {
    0x0000_0001
}

/// Human-readable build identity (crate version + git short sha + dirty).
/// Mirrors smooth_core_build_id() so the C++ Effect Controls "build" label
/// can append GPU build info when the GPU path is enabled.
static BUILD_ID: &str = concat!(env!("CARGO_PKG_VERSION"), "+scaffold\0");

#[no_mangle]
pub extern "C" fn smooth_gpu_build_id() -> *const core::ffi::c_char {
    BUILD_ID.as_ptr() as *const core::ffi::c_char
}

/// Status returned by smooth_gpu_init().
#[repr(u32)]
#[allow(dead_code)]
pub enum InitStatus {
    Ok                  = 0,
    NoAdapter           = 1,
    DeviceCreateFailed  = 2,
}

/// Lazily initialise the wgpu instance / adapter / device. Returns Ok on
/// success or a NoAdapter/DeviceCreateFailed code on failure. The C++ side
/// can call this on plugin load to gate the GPU path; if it returns non-zero
/// the caller falls back to the CPU Rust core (`smooth_core_*`).
#[no_mangle]
pub extern "C" fn smooth_gpu_init() -> u32 {
    static INIT: Once = Once::new();
    static mut STATUS: u32 = InitStatus::Ok as u32;

    INIT.call_once(|| {
        let result = pollster::block_on(try_init_gpu());
        // SAFETY: STATUS is written exactly once inside Once::call_once.
        unsafe { STATUS = result; }
    });

    // SAFETY: STATUS is only written inside Once::call_once above; subsequent
    // reads are safe because the writer has happened-before by Once semantics.
    unsafe { STATUS }
}

async fn try_init_gpu() -> u32 {
    // wgpu 23: Instance::new takes a value, request_adapter returns Option,
    // request_device returns Result<(Device, Queue), _>.
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::PRIMARY, // Metal/Vulkan/DX12, skip GL/WebGPU
        ..Default::default()
    });

    let adapter = match instance
        .request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        })
        .await
    {
        Some(a) => a,
        None => return InitStatus::NoAdapter as u32,
    };

    match adapter
        .request_device(
            &wgpu::DeviceDescriptor {
                label: Some("smooth_gpu device"),
                required_features: wgpu::Features::empty(),
                required_limits: wgpu::Limits::downlevel_defaults(),
                memory_hints: wgpu::MemoryHints::Performance,
            },
            None,
        )
        .await
    {
        Ok(_) => InitStatus::Ok as u32,
        Err(_) => InitStatus::DeviceCreateFailed as u32,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_packs_zero_one() {
        assert_eq!(smooth_gpu_version(), 0x0000_0001);
    }

    #[test]
    fn build_id_is_null_terminated() {
        let ptr = smooth_gpu_build_id();
        // walk until we hit the trailing nul
        let mut len = 0;
        unsafe {
            while *ptr.add(len) != 0 { len += 1; if len > 256 { panic!("missing nul"); } }
        }
        assert!(len > 0);
    }

    /// Liveness probe — should succeed on any modern macOS via Metal.
    /// On Linux without a Vulkan ICD or in headless CI this can return
    /// NoAdapter; gate via SMOOTH_GPU_SKIP_INIT_TEST=1 in those environments.
    #[test]
    fn init_can_acquire_a_device_on_this_host() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() {
            return;
        }
        let status = smooth_gpu_init();
        assert_eq!(status, InitStatus::Ok as u32,
                   "smooth_gpu_init returned non-Ok ({status}); set SMOOTH_GPU_SKIP_INIT_TEST=1 to skip");
    }
}
