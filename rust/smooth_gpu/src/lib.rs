// smooth_gpu — GPU acceleration for the smooth OFX plugin via wgpu.
//
// Phase B: passthrough kernel. The crate now owns a lazily-initialised
// GpuContext (instance/adapter/device/queue + a passthrough compute pipeline)
// behind OnceCell<Mutex<...>>; the C++ side calls smooth_gpu_passthrough_u32()
// to round-trip a u32 buffer through the GPU and confirm byte-identical
// output. This is the foundation that the real preprocess / scan-blend
// kernels (Phase 3-F) will reuse.
//
// Cross-platform notes:
// - macOS:   wgpu Metal backend. No additional system libs needed.
// - Windows: wgpu DX12 backend (or Vulkan if available). Links to system
//            d3d12.lib / dxgi.lib at the OS level via wgpu's runtime loader,
//            so static lib stays free of import deps.
// - Linux:   wgpu Vulkan backend. The OFX bundle does NOT bundle Vulkan;
//            the host system must have a Vulkan driver installed
//            (mesa-vulkan-drivers or equivalent).

use std::sync::{Mutex, OnceLock};

/// Linkage / ABI probe.
/// Upper 16 bits = major (0 = pre-release scaffolding), lower 16 bits = FFI
/// revision. Bump the lower 16 bits whenever any FFI struct changes layout.
#[no_mangle]
pub extern "C" fn smooth_gpu_version() -> u32 {
    0x0000_0002 // bumped: passthrough entry point added
}

/// Human-readable build identity (crate version + git short sha + dirty).
/// Mirrors smooth_core_build_id() so the C++ Effect Controls "build" label
/// can append GPU build info when the GPU path is enabled.
static BUILD_ID: &str = concat!(env!("CARGO_PKG_VERSION"), "+scaffold\0");

#[no_mangle]
pub extern "C" fn smooth_gpu_build_id() -> *const core::ffi::c_char {
    BUILD_ID.as_ptr() as *const core::ffi::c_char
}

/// Status codes. Public so tests + the C++ side can match symbolically.
#[repr(u32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Status {
    Ok                  = 0,
    NoAdapter           = 1,
    DeviceCreateFailed  = 2,
    PipelineFailed      = 3,
    DispatchFailed      = 4,
    NullPointer         = 5,
}

/// Cached GPU context. One instance per process.
struct GpuContext {
    device: wgpu::Device,
    queue:  wgpu::Queue,
    passthrough_pipeline: wgpu::ComputePipeline,
    passthrough_bgl:      wgpu::BindGroupLayout,
}

static CTX: OnceLock<Mutex<Result<GpuContext, Status>>> = OnceLock::new();

fn ctx() -> &'static Mutex<Result<GpuContext, Status>> {
    CTX.get_or_init(|| Mutex::new(pollster::block_on(build_context())))
}

async fn build_context() -> Result<GpuContext, Status> {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::PRIMARY,
        ..Default::default()
    });

    let adapter = instance
        .request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        })
        .await
        .ok_or(Status::NoAdapter)?;

    let (device, queue) = adapter
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
        .map_err(|_| Status::DeviceCreateFailed)?;

    // Passthrough compute pipeline.
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("smooth_gpu passthrough.wgsl"),
        source: wgpu::ShaderSource::Wgsl(include_str!("passthrough.wgsl").into()),
    });

    let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("smooth_gpu passthrough bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry {
                binding: 0, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: true },
                    has_dynamic_offset: false, min_binding_size: None,
                },
                count: None,
            },
            wgpu::BindGroupLayoutEntry {
                binding: 1, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false, min_binding_size: None,
                },
                count: None,
            },
        ],
    });
    let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("smooth_gpu passthrough layout"),
        bind_group_layouts: &[&bgl],
        push_constant_ranges: &[],
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("smooth_gpu passthrough pipeline"),
        layout: Some(&layout),
        module: &shader,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });

    Ok(GpuContext {
        device, queue,
        passthrough_pipeline: pipeline,
        passthrough_bgl: bgl,
    })
}

/// Lazy GPU init probe (kept for backwards compat with Phase A users).
/// Returns Status::Ok on success.
#[no_mangle]
pub extern "C" fn smooth_gpu_init() -> u32 {
    match &*ctx().lock().unwrap() {
        Ok(_)  => Status::Ok          as u32,
        Err(s) => *s                  as u32,
    }
}

/// Round-trip `len` u32 elements through the GPU passthrough kernel.
/// Returns Status::Ok on success and writes `dst[0..len]`. Reads `src[0..len]`.
///
/// # Safety
/// `src` must be a valid pointer to at least `len` u32 elements; `dst` must
/// be a valid pointer to at least `len` writable u32 elements. They must
/// not alias.
#[no_mangle]
pub unsafe extern "C" fn smooth_gpu_passthrough_u32(
    src: *const u32,
    dst: *mut u32,
    len: usize,
) -> u32 {
    if src.is_null() || dst.is_null() {
        return Status::NullPointer as u32;
    }
    if len == 0 {
        return Status::Ok as u32;
    }

    let guard = ctx().lock().unwrap();
    let cx = match &*guard {
        Ok(c)  => c,
        Err(s) => return *s as u32,
    };

    // Stage src into a GPU-visible storage buffer.
    let bytes = (len * std::mem::size_of::<u32>()) as u64;
    let src_slice: &[u32] = std::slice::from_raw_parts(src, len);
    let src_buf = wgpu_util_create_buffer_init(
        &cx.device,
        Some("smooth_gpu passthrough src"),
        bytemuck::cast_slice(src_slice),
        wgpu::BufferUsages::STORAGE,
    );

    // GPU-side dst (storage), and a CPU-readable staging buffer for read-back.
    let dst_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu passthrough dst"),
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu passthrough readback"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    let bg = cx.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("smooth_gpu passthrough bg"),
        layout: &cx.passthrough_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: dst_buf.as_entire_binding() },
        ],
    });

    let mut enc = cx.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("smooth_gpu passthrough enc"),
    });
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("smooth_gpu passthrough pass"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&cx.passthrough_pipeline);
        pass.set_bind_group(0, &bg, &[]);
        let workgroups = ((len as u32) + 63) / 64;
        pass.dispatch_workgroups(workgroups, 1, 1);
    }
    enc.copy_buffer_to_buffer(&dst_buf, 0, &read_buf, 0, bytes);
    cx.queue.submit(Some(enc.finish()));

    // Map and read back.
    let slice = read_buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| {
        let _ = tx.send(r);
    });
    cx.device.poll(wgpu::Maintain::Wait);
    if rx.recv().is_err() {
        return Status::DispatchFailed as u32;
    }

    let view = slice.get_mapped_range();
    let dst_slice: &mut [u32] = std::slice::from_raw_parts_mut(dst, len);
    dst_slice.copy_from_slice(bytemuck::cast_slice(&view));
    drop(view);
    read_buf.unmap();

    Status::Ok as u32
}

// Tiny shim around wgpu::util::DeviceExt::create_buffer_init, kept inside the
// crate so we don't pull in `wgpu::util::DeviceExt` at every call site.
fn wgpu_util_create_buffer_init(
    device: &wgpu::Device,
    label: Option<&str>,
    contents: &[u8],
    usage: wgpu::BufferUsages,
) -> wgpu::Buffer {
    use wgpu::util::DeviceExt;
    device.create_buffer_init(&wgpu::util::BufferInitDescriptor { label, contents, usage })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_packs_zero_two() {
        assert_eq!(smooth_gpu_version(), 0x0000_0002);
    }

    #[test]
    fn build_id_is_null_terminated() {
        let ptr = smooth_gpu_build_id();
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
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        assert_eq!(smooth_gpu_init(), Status::Ok as u32);
    }

    /// End-to-end: send 1024 u32 through the GPU passthrough kernel and
    /// verify byte-identical output. Validates buffer upload, dispatch,
    /// readback, and bind group wiring.
    #[test]
    fn passthrough_round_trips_byte_identical() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let n: usize = 1024;
        let src: Vec<u32> = (0..n).map(|i| (i as u32).wrapping_mul(0x9E3779B1)).collect();
        let mut dst: Vec<u32> = vec![0; n];
        let st = unsafe {
            smooth_gpu_passthrough_u32(src.as_ptr(), dst.as_mut_ptr(), n)
        };
        assert_eq!(st, Status::Ok as u32, "passthrough returned {st}");
        assert_eq!(src, dst, "GPU passthrough output differs from input");
    }
}
