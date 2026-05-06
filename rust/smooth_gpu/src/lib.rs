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
    0x0000_0006 // bumped: smooth_gpu_preprocess_ofx_u8 added
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

/// SmoothBbox layout matches smooth_core's smooth_bbox_t. We re-declare it
/// here so the GPU path stays standalone (USE_RUST_CORE and USE_GPU_CORE
/// are mutually exclusive at link time — see CMakeLists.txt comment).
#[repr(C)]
#[derive(Copy, Clone, Default, PartialEq, Eq, Debug)]
pub struct SmoothBbox {
    pub top:    i32,
    pub left:   i32,
    pub right:  i32,
    pub bottom: i32,
}

/// Cached GPU context. One instance per process.
struct GpuContext {
    device: wgpu::Device,
    queue:  wgpu::Queue,

    // Passthrough (sanity / harness probe).
    passthrough_pipeline: wgpu::ComputePipeline,
    passthrough_bgl:      wgpu::BindGroupLayout,

    // Preprocess (Phase 3-F1) for u8 ARGB pixels.
    preprocess_u8_pipeline: wgpu::ComputePipeline,
    preprocess_u8_bgl:      wgpu::BindGroupLayout,

    // mode_flg detection (Phase 3-F2A) for u8 ARGB pixels.
    mode_flg_u8_pipeline: wgpu::ComputePipeline,
    mode_flg_u8_bgl:      wgpu::BindGroupLayout,

    // link8_square center (Phase 3-F2B) for u8 ARGB pixels.
    link8_square_center_u8_pipeline: wgpu::ComputePipeline,
    link8_square_center_u8_bgl:      wgpu::BindGroupLayout,

    // OFX-native preprocess (Phase 3-F4) for u8 RGBA pixels.
    preprocess_ofx_u8_pipeline: wgpu::ComputePipeline,
    preprocess_ofx_u8_bgl:      wgpu::BindGroupLayout,
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

    // mode_flg detection (u8 ARGB) compute pipeline (Phase 3-F2A).
    // Same bgl shape as preprocess (3 bindings: src ro, mode_out rw, params)
    // so we declare it inline. Borrows the same pixel ARGB packing convention.
    let mode_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("smooth_gpu mode_flg_u8.wgsl"),
        source: wgpu::ShaderSource::Wgsl(include_str!("mode_flg_u8.wgsl").into()),
    });
    let mode_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("smooth_gpu mode_flg_u8 bgl"),
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
            wgpu::BindGroupLayoutEntry {
                binding: 2, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false, min_binding_size: None,
                },
                count: None,
            },
        ],
    });
    let mode_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("smooth_gpu mode_flg_u8 layout"),
        bind_group_layouts: &[&mode_bgl],
        push_constant_ranges: &[],
    });
    let mode_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("smooth_gpu mode_flg_u8 pipeline"),
        layout: Some(&mode_layout),
        module: &mode_shader,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });

    // OFX-native preprocess (u8 RGBA) compute pipeline (Phase 3-F4).
    // Same bgl shape as preprocess_u8 (AE ARGB) but a different shader so
    // we don't need to swizzle in C++ before/after the GPU call.
    let pre_ofx_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8.wgsl"),
        source: wgpu::ShaderSource::Wgsl(include_str!("preprocess_ofx_u8.wgsl").into()),
    });
    let pre_ofx_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: true },  has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: false }, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: false }, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 3, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
        ],
    });
    let pre_ofx_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 layout"),
        bind_group_layouts: &[&pre_ofx_bgl],
        push_constant_ranges: &[],
    });
    let pre_ofx_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 pipeline"),
        layout: Some(&pre_ofx_layout),
        module: &pre_ofx_shader,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });

    // link8_square center compute pipeline (Phase 3-F2B).
    let l8sq_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("smooth_gpu link8_square_center_u8.wgsl"),
        source: wgpu::ShaderSource::Wgsl(include_str!("link8_square_center_u8.wgsl").into()),
    });
    let l8sq_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("smooth_gpu link8_square_center_u8 bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry { binding: 0, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: true },  has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 1, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: false }, has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 2, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: true },  has_dynamic_offset: false, min_binding_size: None }, count: None },
            wgpu::BindGroupLayoutEntry { binding: 3, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }, count: None },
        ],
    });
    let l8sq_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("smooth_gpu link8_square_center_u8 layout"),
        bind_group_layouts: &[&l8sq_bgl],
        push_constant_ranges: &[],
    });
    let l8sq_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("smooth_gpu link8_square_center_u8 pipeline"),
        layout: Some(&l8sq_layout),
        module: &l8sq_shader,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });

    // Preprocess (u8 ARGB) compute pipeline.
    let pre_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("smooth_gpu preprocess_u8.wgsl"),
        source: wgpu::ShaderSource::Wgsl(include_str!("preprocess_u8.wgsl").into()),
    });
    let pre_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("smooth_gpu preprocess_u8 bgl"),
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
            wgpu::BindGroupLayoutEntry {
                binding: 2, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false, min_binding_size: None,
                },
                count: None,
            },
            wgpu::BindGroupLayoutEntry {
                binding: 3, visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false, min_binding_size: None,
                },
                count: None,
            },
        ],
    });
    let pre_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("smooth_gpu preprocess_u8 layout"),
        bind_group_layouts: &[&pre_bgl],
        push_constant_ranges: &[],
    });
    let pre_pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("smooth_gpu preprocess_u8 pipeline"),
        layout: Some(&pre_layout),
        module: &pre_shader,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });

    Ok(GpuContext {
        device, queue,
        passthrough_pipeline: pipeline,
        passthrough_bgl: bgl,
        preprocess_u8_pipeline: pre_pipeline,
        preprocess_u8_bgl: pre_bgl,
        mode_flg_u8_pipeline: mode_pipeline,
        mode_flg_u8_bgl: mode_bgl,
        link8_square_center_u8_pipeline: l8sq_pipeline,
        link8_square_center_u8_bgl: l8sq_bgl,
        preprocess_ofx_u8_pipeline: pre_ofx_pipeline,
        preprocess_ofx_u8_bgl: pre_ofx_bgl,
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

/// Phase 3-F1: u8 (ARGB) preprocess on GPU.
/// Round-trips an image through the preprocess kernel and writes the result
/// to `out_ptr`. `bbox_out` receives the SmoothBbox-shaped result (matching
/// smooth_core::pre_process semantics).
///
/// `src_ptr` and `out_ptr` may alias (same pointer is fine for in-place
/// semantics from the caller's perspective; we always stage src to a GPU
/// buffer and write out_ptr from the GPU readback).
///
/// `rowbytes` must equal `width * 4`; tight buffers only for now (matches the
/// way smooth_core_preprocess_u8 is currently called from the OFX C++ side).
///
/// # Safety
/// `src_ptr` must point to at least `(rowbytes/4) * height` u32 values.
/// `out_ptr` must point to at least the same number of writable u32 slots.
/// `bbox_out` must be a valid pointer to a SmoothBbox (16 bytes).
#[no_mangle]
pub unsafe extern "C" fn smooth_gpu_preprocess_u8(
    src_ptr: *const u32,
    out_ptr: *mut u32,
    rowbytes: i32,
    height: i32,
    is_white_trans: i32,
    bbox_out: *mut SmoothBbox,
) -> u32 {
    if src_ptr.is_null() || out_ptr.is_null() || bbox_out.is_null() {
        return Status::NullPointer as u32;
    }
    if rowbytes <= 0 || height <= 0 {
        // Degenerate inputs: hand back the "no pixels found" sentinel.
        *bbox_out = SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 };
        return Status::Ok as u32;
    }
    let width  = (rowbytes as usize) / 4;
    let height_u = height as usize;
    let n = width * height_u;
    if n == 0 {
        *bbox_out = SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 };
        return Status::Ok as u32;
    }

    let guard = ctx().lock().unwrap();
    let cx = match &*guard {
        Ok(c)  => c,
        Err(s) => return *s as u32,
    };

    let bytes = (n * 4) as u64;
    let src_slice: &[u32] = std::slice::from_raw_parts(src_ptr, n);

    let src_buf = wgpu_util_create_buffer_init(
        &cx.device,
        Some("smooth_gpu preprocess_u8 src"),
        bytemuck::cast_slice(src_slice),
        wgpu::BufferUsages::STORAGE,
    );
    let dst_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_u8 dst"),
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });

    // bbox storage buffer seeded to {INT_MAX, INT_MAX, -1, -1}.
    let bbox_seed: [i32; 4] = [i32::MAX, i32::MAX, -1, -1];
    let bbox_buf = wgpu_util_create_buffer_init(
        &cx.device,
        Some("smooth_gpu preprocess_u8 bbox"),
        bytemuck::cast_slice(&bbox_seed),
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );

    // Uniform params.
    #[repr(C)]
    #[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
    struct Params { width: u32, height: u32, is_white_trans: u32, _pad: u32 }
    let params = Params {
        width: width as u32,
        height: height as u32,
        is_white_trans: if is_white_trans != 0 { 1 } else { 0 },
        _pad: 0,
    };
    let params_buf = wgpu_util_create_buffer_init(
        &cx.device,
        Some("smooth_gpu preprocess_u8 params"),
        bytemuck::bytes_of(&params),
        wgpu::BufferUsages::UNIFORM,
    );

    // Readback buffers.
    let read_dst = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_u8 readback dst"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let read_bbox = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_u8 readback bbox"),
        size: 16,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    let bg = cx.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("smooth_gpu preprocess_u8 bg"),
        layout: &cx.preprocess_u8_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: dst_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 2, resource: bbox_buf.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 3, resource: params_buf.as_entire_binding() },
        ],
    });

    let mut enc = cx.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("smooth_gpu preprocess_u8 enc"),
    });
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("smooth_gpu preprocess_u8 pass"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&cx.preprocess_u8_pipeline);
        pass.set_bind_group(0, &bg, &[]);
        let wx = ((width as u32) + 7) / 8;
        let wy = ((height as u32) + 7) / 8;
        pass.dispatch_workgroups(wx, wy, 1);
    }
    enc.copy_buffer_to_buffer(&dst_buf,  0, &read_dst,  0, bytes);
    enc.copy_buffer_to_buffer(&bbox_buf, 0, &read_bbox, 0, 16);
    cx.queue.submit(Some(enc.finish()));

    // Read back dst.
    let dst_slice = read_dst.slice(..);
    let (tx_d, rx_d) = std::sync::mpsc::channel();
    dst_slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx_d.send(r); });
    let bb_slice = read_bbox.slice(..);
    let (tx_b, rx_b) = std::sync::mpsc::channel();
    bb_slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx_b.send(r); });
    cx.device.poll(wgpu::Maintain::Wait);
    if rx_d.recv().is_err() || rx_b.recv().is_err() {
        return Status::DispatchFailed as u32;
    }

    let dst_view = dst_slice.get_mapped_range();
    let out_slice: &mut [u32] = std::slice::from_raw_parts_mut(out_ptr, n);
    out_slice.copy_from_slice(bytemuck::cast_slice(&dst_view));
    drop(dst_view);
    read_dst.unmap();

    let bb_view = bb_slice.get_mapped_range();
    let bb: &[i32] = bytemuck::cast_slice(&bb_view);
    let (top_a, left_a, right_a, bottom_a) = (bb[0], bb[1], bb[2], bb[3]);
    drop(bb_view);
    read_bbox.unmap();

    // Project atomic-min/max raw values onto the smooth_core SmoothBbox shape.
    let bbox = if top_a == i32::MAX {
        SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 }
    } else {
        SmoothBbox {
            top: top_a, left: left_a,
            right: right_a + 1,
            bottom: bottom_a + 1,
        }
    };
    *bbox_out = bbox;

    Status::Ok as u32
}

/// Phase 3-F2A: per-pixel mode_flg detection on GPU.
/// Writes a u32 per pixel into `mode_out`, with the 4-bit mode_flg in the
/// low bits (matches smooth_core::process_row_range's pixel-level dispatch
/// key). Reads from `src_ptr`. Border pixels (the 1-pixel edge) get
/// mode_flg = 0, mirroring the CPU side's extent inward clamp.
///
/// `rowbytes` must equal `width * 4` (tight buffer). `range` is the same
/// integer threshold passed via smooth_core_process_row_range_u8.
///
/// # Safety
/// `src_ptr` must point to at least `(rowbytes/4) * height` u32 values.
/// `mode_out` must point to at least the same number of writable u32 slots.
#[no_mangle]
pub unsafe extern "C" fn smooth_gpu_mode_flg_u8(
    src_ptr:  *const u32,
    mode_out: *mut u32,
    rowbytes: i32,
    height:   i32,
    range:    u32,
) -> u32 {
    if src_ptr.is_null() || mode_out.is_null() {
        return Status::NullPointer as u32;
    }
    if rowbytes <= 0 || height <= 0 {
        return Status::Ok as u32;
    }
    let width  = (rowbytes as usize) / 4;
    let height_u = height as usize;
    let n = width * height_u;
    if n == 0 { return Status::Ok as u32; }

    let guard = ctx().lock().unwrap();
    let cx = match &*guard {
        Ok(c)  => c,
        Err(s) => return *s as u32,
    };

    let bytes = (n * 4) as u64;
    let src_slice: &[u32] = std::slice::from_raw_parts(src_ptr, n);

    let src_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu mode_flg_u8 src"),
        bytemuck::cast_slice(src_slice),
        wgpu::BufferUsages::STORAGE,
    );
    let mode_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu mode_flg_u8 mode"),
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });

    #[repr(C)]
    #[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
    struct Params { width: u32, height: u32, range: u32, _pad: u32 }
    let params = Params { width: width as u32, height: height as u32, range, _pad: 0 };
    let params_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu mode_flg_u8 params"),
        bytemuck::bytes_of(&params),
        wgpu::BufferUsages::UNIFORM,
    );

    let read_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu mode_flg_u8 readback"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    let bg = cx.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("smooth_gpu mode_flg_u8 bg"),
        layout: &cx.mode_flg_u8_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src_buf.as_entire_binding()    },
            wgpu::BindGroupEntry { binding: 1, resource: mode_buf.as_entire_binding()   },
            wgpu::BindGroupEntry { binding: 2, resource: params_buf.as_entire_binding() },
        ],
    });

    let mut enc = cx.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("smooth_gpu mode_flg_u8 enc"),
    });
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("smooth_gpu mode_flg_u8 pass"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&cx.mode_flg_u8_pipeline);
        pass.set_bind_group(0, &bg, &[]);
        let wx = ((width as u32) + 7) / 8;
        let wy = ((height as u32) + 7) / 8;
        pass.dispatch_workgroups(wx, wy, 1);
    }
    enc.copy_buffer_to_buffer(&mode_buf, 0, &read_buf, 0, bytes);
    cx.queue.submit(Some(enc.finish()));

    let slice = read_buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
    cx.device.poll(wgpu::Maintain::Wait);
    if rx.recv().is_err() { return Status::DispatchFailed as u32; }

    let view = slice.get_mapped_range();
    let out: &mut [u32] = std::slice::from_raw_parts_mut(mode_out, n);
    out.copy_from_slice(bytemuck::cast_slice(&view));
    drop(view);
    read_buf.unmap();

    Status::Ok as u32
}

/// Phase 3-F4: u8 (OFX RGBA layout) preprocess on GPU. Counterpart of
/// `smooth_gpu_preprocess_u8` (which uses AE ARGB layout); this one
/// matches OFX's `OfxRGBAColourB` layout natively, so the C++ render path
/// can call it without a swizzle pre/post pass. Other than the byte
/// layout, the contract is identical.
///
/// # Safety
/// Same as `smooth_gpu_preprocess_u8`: pointers must each cover at least
/// `(rowbytes/4) * height` u32 slots; bbox_out must be a writable
/// SmoothBbox.
#[no_mangle]
pub unsafe extern "C" fn smooth_gpu_preprocess_ofx_u8(
    src_ptr: *const u32,
    out_ptr: *mut u32,
    rowbytes: i32,
    height: i32,
    is_white_trans: i32,
    bbox_out: *mut SmoothBbox,
) -> u32 {
    if src_ptr.is_null() || out_ptr.is_null() || bbox_out.is_null() {
        return Status::NullPointer as u32;
    }
    if rowbytes <= 0 || height <= 0 {
        *bbox_out = SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 };
        return Status::Ok as u32;
    }
    let width  = (rowbytes as usize) / 4;
    let height_u = height as usize;
    let n = width * height_u;
    if n == 0 {
        *bbox_out = SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 };
        return Status::Ok as u32;
    }

    let guard = ctx().lock().unwrap();
    let cx = match &*guard {
        Ok(c)  => c,
        Err(s) => return *s as u32,
    };

    let bytes = (n * 4) as u64;
    let src_slice: &[u32] = std::slice::from_raw_parts(src_ptr, n);

    let src_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu preprocess_ofx_u8 src"),
        bytemuck::cast_slice(src_slice),
        wgpu::BufferUsages::STORAGE,
    );
    let dst_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 dst"),
        size: bytes,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });

    let bbox_seed: [i32; 4] = [i32::MAX, i32::MAX, -1, -1];
    let bbox_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu preprocess_ofx_u8 bbox"),
        bytemuck::cast_slice(&bbox_seed),
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );

    #[repr(C)]
    #[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
    struct Params { width: u32, height: u32, is_white_trans: u32, _pad: u32 }
    let params = Params {
        width: width as u32,
        height: height as u32,
        is_white_trans: if is_white_trans != 0 { 1 } else { 0 },
        _pad: 0,
    };
    let params_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu preprocess_ofx_u8 params"),
        bytemuck::bytes_of(&params),
        wgpu::BufferUsages::UNIFORM,
    );

    let read_dst = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 readback dst"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let read_bbox = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 readback bbox"),
        size: 16,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    let bg = cx.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 bg"),
        layout: &cx.preprocess_ofx_u8_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src_buf.as_entire_binding()    },
            wgpu::BindGroupEntry { binding: 1, resource: dst_buf.as_entire_binding()    },
            wgpu::BindGroupEntry { binding: 2, resource: bbox_buf.as_entire_binding()   },
            wgpu::BindGroupEntry { binding: 3, resource: params_buf.as_entire_binding() },
        ],
    });

    let mut enc = cx.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("smooth_gpu preprocess_ofx_u8 enc"),
    });
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("smooth_gpu preprocess_ofx_u8 pass"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&cx.preprocess_ofx_u8_pipeline);
        pass.set_bind_group(0, &bg, &[]);
        let wx = ((width as u32) + 7) / 8;
        let wy = ((height as u32) + 7) / 8;
        pass.dispatch_workgroups(wx, wy, 1);
    }
    enc.copy_buffer_to_buffer(&dst_buf,  0, &read_dst,  0, bytes);
    enc.copy_buffer_to_buffer(&bbox_buf, 0, &read_bbox, 0, 16);
    cx.queue.submit(Some(enc.finish()));

    let dst_slice = read_dst.slice(..);
    let (tx_d, rx_d) = std::sync::mpsc::channel();
    dst_slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx_d.send(r); });
    let bb_slice = read_bbox.slice(..);
    let (tx_b, rx_b) = std::sync::mpsc::channel();
    bb_slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx_b.send(r); });
    cx.device.poll(wgpu::Maintain::Wait);
    if rx_d.recv().is_err() || rx_b.recv().is_err() {
        return Status::DispatchFailed as u32;
    }

    let dst_view = dst_slice.get_mapped_range();
    let out_slice: &mut [u32] = std::slice::from_raw_parts_mut(out_ptr, n);
    out_slice.copy_from_slice(bytemuck::cast_slice(&dst_view));
    drop(dst_view);
    read_dst.unmap();

    let bb_view = bb_slice.get_mapped_range();
    let bb: &[i32] = bytemuck::cast_slice(&bb_view);
    let (top_a, left_a, right_a, bottom_a) = (bb[0], bb[1], bb[2], bb[3]);
    drop(bb_view);
    read_bbox.unmap();

    let bbox = if top_a == i32::MAX {
        SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 }
    } else {
        SmoothBbox {
            top: top_a, left: left_a,
            right: right_a + 1,
            bottom: bottom_a + 1,
        }
    };
    *bbox_out = bbox;

    Status::Ok as u32
}

/// Phase 3-F2B: link8_square center (4-diagonal blend + average) on GPU.
/// Operates only on pixels where `mode_flg[i] & 0xF == 15`, leaves others
/// untouched. Caller must pre-fill `dst[]` with `src[]` (memcpy) so the
/// non-15 pixels carry through.
///
/// Outside expansion (link8_square_blend_outside) is intentionally not
/// included — that's a row/column-sequential operation deferred to a
/// later sub-phase.
///
/// # Safety
/// All three buffers must hold at least `(rowbytes/4) * height` u32 values
/// and not alias.
#[no_mangle]
pub unsafe extern "C" fn smooth_gpu_link8_square_center_u8(
    src_ptr:      *const u32,
    dst_ptr:      *mut u32,
    mode_flg_ptr: *const u32,
    rowbytes:     i32,
    height:       i32,
    range:        u32,
) -> u32 {
    if src_ptr.is_null() || dst_ptr.is_null() || mode_flg_ptr.is_null() {
        return Status::NullPointer as u32;
    }
    if rowbytes <= 0 || height <= 0 { return Status::Ok as u32; }
    let width    = (rowbytes as usize) / 4;
    let height_u = height as usize;
    let n        = width * height_u;
    if n == 0 { return Status::Ok as u32; }

    let guard = ctx().lock().unwrap();
    let cx = match &*guard {
        Ok(c)  => c,
        Err(s) => return *s as u32,
    };

    let bytes = (n * 4) as u64;
    let src_slice:  &[u32] = std::slice::from_raw_parts(src_ptr, n);
    let dst_slice:  &[u32] = std::slice::from_raw_parts(dst_ptr as *const u32, n);
    let mode_slice: &[u32] = std::slice::from_raw_parts(mode_flg_ptr, n);

    let src_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu link8sq src"),
        bytemuck::cast_slice(src_slice),
        wgpu::BufferUsages::STORAGE,
    );
    // Seed dst on GPU with caller's pre-filled dst (so non-15 pixels survive).
    let dst_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu link8sq dst (seed)"),
        bytemuck::cast_slice(dst_slice),
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );
    let mode_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu link8sq mode_flg"),
        bytemuck::cast_slice(mode_slice),
        wgpu::BufferUsages::STORAGE,
    );

    #[repr(C)]
    #[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
    struct Params { width: u32, height: u32, range: u32, _pad: u32 }
    let params = Params { width: width as u32, height: height as u32, range, _pad: 0 };
    let params_buf = wgpu_util_create_buffer_init(
        &cx.device, Some("smooth_gpu link8sq params"),
        bytemuck::bytes_of(&params),
        wgpu::BufferUsages::UNIFORM,
    );

    let read_buf = cx.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("smooth_gpu link8sq readback"),
        size: bytes,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });

    let bg = cx.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("smooth_gpu link8sq bg"),
        layout: &cx.link8_square_center_u8_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src_buf.as_entire_binding()    },
            wgpu::BindGroupEntry { binding: 1, resource: dst_buf.as_entire_binding()    },
            wgpu::BindGroupEntry { binding: 2, resource: mode_buf.as_entire_binding()   },
            wgpu::BindGroupEntry { binding: 3, resource: params_buf.as_entire_binding() },
        ],
    });

    let mut enc = cx.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("smooth_gpu link8sq enc"),
    });
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("smooth_gpu link8sq pass"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&cx.link8_square_center_u8_pipeline);
        pass.set_bind_group(0, &bg, &[]);
        let wx = ((width as u32) + 7) / 8;
        let wy = ((height as u32) + 7) / 8;
        pass.dispatch_workgroups(wx, wy, 1);
    }
    enc.copy_buffer_to_buffer(&dst_buf, 0, &read_buf, 0, bytes);
    cx.queue.submit(Some(enc.finish()));

    let slice = read_buf.slice(..);
    let (tx, rx) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
    cx.device.poll(wgpu::Maintain::Wait);
    if rx.recv().is_err() { return Status::DispatchFailed as u32; }

    let view = slice.get_mapped_range();
    let out: &mut [u32] = std::slice::from_raw_parts_mut(dst_ptr, n);
    out.copy_from_slice(bytemuck::cast_slice(&view));
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
    fn version_packs_zero_six() {
        assert_eq!(smooth_gpu_version(), 0x0000_0006);
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

    // CPU reference port of smooth_core::pre_process for u8 ARGB.
    // Local copy so the test stays standalone (smooth_core can't be a Rust
    // path-dep — see Cargo.toml). Source of truth is
    // smooth-ae/rust/smooth_core/src/preprocess.rs.
    fn cpu_preprocess_u8_reference(
        pixels: &mut [u32],
        width: usize,
        height: usize,
        is_white_trans: bool,
    ) -> SmoothBbox {
        let null: u32 = 0;
        let mut top: i32    = 0;
        let mut left: i32   = width as i32;
        let mut right: i32  = 0;
        let mut bottom: i32 = 0;
        let mut top_found  = false;
        let mut left_found = false;
        let mut t: usize = 0;

        let is_white = |p: u32| -> bool { ((p >> 8) & 0x00FF_FFFF) == 0x00FF_FFFF };
        let alpha_zero = |p: u32| -> bool { (p & 0xFF) == 0 };

        if is_white_trans {
            for j in 0..height {
                if !top_found { top = j as i32; }
                for i in 0..width {
                    let p = pixels[t];
                    if is_white(p) {
                        pixels[t] = null;
                    } else if !alpha_zero(p) {
                        top_found = true; left_found = true;
                        let ii = i as i32; let jj = j as i32;
                        if left  > ii { left  = ii; }
                        if right < ii { right = ii; }
                        if bottom < jj { bottom = jj; }
                    }
                    t += 1;
                }
            }
        } else {
            for j in 0..height {
                if !top_found { top = j as i32; }
                for i in 0..width {
                    let p = pixels[t];
                    if !is_white(p) && !alpha_zero(p) {
                        top_found = true; left_found = true;
                        let ii = i as i32; let jj = j as i32;
                        if left  > ii { left  = ii; }
                        if right < ii { right = ii; }
                        if bottom < jj { bottom = jj; }
                    }
                    t += 1;
                }
            }
        }
        SmoothBbox {
            top:    if top_found  { top  } else { 0 },
            left:   if left_found { left } else { 0 },
            right:  right + 1,
            bottom: bottom + 1,
        }
    }

    fn pack_argb(a: u8, r: u8, g: u8, b: u8) -> u32 {
        (a as u32) | ((r as u32) << 8) | ((g as u32) << 16) | ((b as u32) << 24)
    }

    /// Compare GPU preprocess output to the CPU reference on a deterministic
    /// 64x32 image with a mix of white / transparent / coloured pixels.
    /// Both `is_white_trans` branches must produce byte-identical buffers
    /// AND identical SmoothBbox output.
    #[test]
    fn preprocess_u8_matches_cpu_reference() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let w = 64usize;
        let h = 32usize;
        let mut img: Vec<u32> = Vec::with_capacity(w * h);
        for j in 0..h {
            for i in 0..w {
                let pixel = if (i + j) % 7 == 0 {
                    pack_argb(0xFF, 0xFF, 0xFF, 0xFF)              // white
                } else if (i + j) % 5 == 0 {
                    pack_argb(0x00, 0x00, 0x00, 0x00)              // transparent
                } else {
                    pack_argb(0xFF, (i as u8) & 0xFF, (j as u8) & 0xFF, 0x40)
                };
                img.push(pixel);
            }
        }

        for &white_trans in &[false, true] {
            let mut cpu_img = img.clone();
            let cpu_bb = cpu_preprocess_u8_reference(&mut cpu_img, w, h, white_trans);

            let mut gpu_img = img.clone();
            let mut gpu_bb = SmoothBbox::default();
            let st = unsafe {
                smooth_gpu_preprocess_u8(
                    img.as_ptr(),
                    gpu_img.as_mut_ptr(),
                    (w * 4) as i32,
                    h as i32,
                    if white_trans { 1 } else { 0 },
                    &mut gpu_bb,
                )
            };
            assert_eq!(st, Status::Ok as u32, "white_trans={white_trans} status={st}");
            assert_eq!(cpu_bb, gpu_bb, "bbox mismatch (white_trans={white_trans})");
            // Find first divergence to give a useful failure message.
            for k in 0..(w * h) {
                if cpu_img[k] != gpu_img[k] {
                    panic!("pixel {k} differs: cpu=0x{:08X} gpu=0x{:08X} (white_trans={white_trans})",
                           cpu_img[k], gpu_img[k]);
                }
            }
        }
    }

    // CPU reference for mode_flg_u8 (mirrors smooth_core::process_row_range
    // up to the 4-bit mode_flg detection — it does not run the corner
    // handlers). Source of truth: smooth-ae/rust/smooth_core/src/{compare,
    // process}.rs.
    fn cpu_mode_flg_u8_reference(
        src: &[u32], width: usize, height: usize, range: u32,
    ) -> Vec<u32> {
        let mut out = vec![0u32; width * height];
        let delta_sum = |a: u32, b: u32| -> u32 {
            let unpack = |p: u32| -> [u32; 4] {
                [p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, (p >> 24) & 0xFF]
            };
            let aa = unpack(a); let bb = unpack(b);
            let d = |x: u32, y: u32| if x > y { x - y } else { y - x };
            d(aa[0], bb[0]) + d(aa[1], bb[1]) + d(aa[2], bb[2]) + d(aa[3], bb[3])
        };
        let cmp = |a: u32, b: u32| delta_sum(a, b) > range;
        for j in 1..height.saturating_sub(1) {
            for i in 1..width.saturating_sub(1) {
                let idx = j * width + i;
                let p_self  = src[idx];
                let p_right = src[idx + 1];
                if p_self != p_right {
                    let p_left = src[idx - 1];
                    let p_top  = src[idx - width];
                    let p_bot  = src[idx + width];
                    let mut m = 0u32;
                    if cmp(p_self, p_right) { m |= 1 << 0; }
                    if cmp(p_self, p_top)   { m |= 1 << 1; }
                    if cmp(p_self, p_bot)   { m |= 1 << 2; }
                    if cmp(p_self, p_left)  { m |= 1 << 3; }
                    out[idx] = m;
                }
            }
        }
        out
    }

    /// Compare GPU mode_flg detection to the CPU reference on a deterministic
    /// 64x32 image with both contiguous regions and sharp transitions.
    #[test]
    fn mode_flg_u8_matches_cpu_reference() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let w = 64usize; let h = 32usize;
        // Diagonal stripes: produces non-trivial mode_flg around edges.
        let mut img: Vec<u32> = Vec::with_capacity(w * h);
        for j in 0..h {
            for i in 0..w {
                let step = (i + j) / 4;
                let p = if step % 2 == 0 {
                    pack_argb(0xFF, 0x00, 0x00, 0x00)
                } else {
                    pack_argb(0xFF, 0xFF, 0xFF, 0xFF)
                };
                img.push(p);
            }
        }
        for &range in &[0u32, 10u32, 100u32, 4080u32] {
            let cpu = cpu_mode_flg_u8_reference(&img, w, h, range);
            let mut gpu = vec![0u32; w * h];
            let st = unsafe {
                smooth_gpu_mode_flg_u8(
                    img.as_ptr(), gpu.as_mut_ptr(),
                    (w * 4) as i32, h as i32, range,
                )
            };
            assert_eq!(st, Status::Ok as u32, "range={range} status={st}");
            for k in 0..(w * h) {
                if cpu[k] != gpu[k] {
                    panic!("mode_flg differs at {k} (range={range}): cpu={:#x} gpu={:#x}",
                           cpu[k], gpu[k]);
                }
            }
        }
    }

    // CPU reference for link8_square center handler.
    // Source of truth: smooth-ae/rust/smooth_core/src/link8.rs link8_square_execute
    // (lines 416-446). Outside expansion intentionally omitted for byte-equality
    // with the GPU kernel which also omits it.
    fn cpu_link8_square_center_reference(
        src: &[u32], dst: &mut [u32], mode_flg: &[u32],
        width: usize, height: usize, range: u32,
    ) {
        let unpack = |p: u32| [p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, (p >> 24) & 0xFF];
        let pack = |c: [u32; 4]| (c[0] & 0xFF) | ((c[1] & 0xFF) << 8) | ((c[2] & 0xFF) << 16) | ((c[3] & 0xFF) << 24);
        let delta_sum = |a: u32, b: u32| -> u32 {
            let aa = unpack(a); let bb = unpack(b);
            let d = |x: u32, y: u32| if x > y { x - y } else { y - x };
            d(aa[0], bb[0]) + d(aa[1], bb[1]) + d(aa[2], bb[2]) + d(aa[3], bb[3])
        };
        let blend_half = |target: u32, refp: u32| -> u32 {
            let max_v: u32 = 0xFF;
            let a_v: u32 = 0x7F;
            let r_v: u32 = 0x80;
            let t = unpack(target); let r = unpack(refp);
            let tp_a = t[0]; let rp_a = r[0];
            let mut out = [0u32; 4];
            if tp_a == max_v && rp_a == max_v {
                out[0] = max_v;
                out[1] = (t[1] * a_v + r[1] * r_v) / max_v;
                out[2] = (t[2] * a_v + r[2] * r_v) / max_v;
                out[3] = (t[3] * a_v + r[3] * r_v) / max_v;
            } else if tp_a == 0 {
                out[0] = (tp_a * a_v + rp_a * r_v) / max_v;
                out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
            } else if rp_a == 0 {
                out[0] = (tp_a * a_v + rp_a * r_v) / max_v;
                out[1] = t[1]; out[2] = t[2]; out[3] = t[3];
            } else {
                out[0] = (tp_a * a_v + rp_a * r_v) / max_v;
                out[1] = (t[1] * a_v + r[1] * r_v) / max_v;
                out[2] = (t[2] * a_v + r[2] * r_v) / max_v;
                out[3] = (t[3] * a_v + r[3] * r_v) / max_v;
            }
            pack(out)
        };

        for j in 1..height.saturating_sub(1) {
            for i in 1..width.saturating_sub(1) {
                let idx = j * width + i;
                if (mode_flg[idx] & 0xF) != 15 { continue; }
                let p_self = src[idx];
                let neighbours = [
                    src[idx - width - 1],   // TL
                    src[idx - width + 1],   // TR
                    src[idx + width + 1],   // BR
                    src[idx + width - 1],   // BL
                ];
                let mut sum = [0u32; 4];
                for k in 0..4 {
                    let temp = if delta_sum(p_self, neighbours[k]) <= range {
                        p_self
                    } else {
                        blend_half(p_self, neighbours[k])
                    };
                    let u = unpack(temp);
                    sum[0] += u[0]; sum[1] += u[1]; sum[2] += u[2]; sum[3] += u[3];
                }
                let avg = [sum[0] >> 2, sum[1] >> 2, sum[2] >> 2, sum[3] >> 2];
                dst[idx] = pack(avg);
            }
        }
    }

    /// link8_square_center kernel byte-identical to CPU reference on a grid
    /// of solid-coloured 5x5 tiles separated by 1-pixel gutters of a
    /// different colour. Many pixels in such an image have mode_flg == 15
    /// (all 4 neighbours differ), exercising the kernel.
    #[test]
    fn link8_square_center_u8_matches_cpu_reference() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let w = 64usize; let h = 32usize;
        // Random-ish content so blend_half hits its mixed-alpha branches too.
        let mut img: Vec<u32> = Vec::with_capacity(w * h);
        for j in 0..h {
            for i in 0..w {
                let r = ((i * 37 + j * 11) & 0xFF) as u8;
                let g = ((i * 13 + j * 41) & 0xFF) as u8;
                let b = ((i * 7  + j * 59) & 0xFF) as u8;
                let a = if (i + j) % 5 == 0 { 0u8 } else { 0xFFu8 };
                img.push(pack_argb(a, r, g, b));
            }
        }
        for &range in &[10u32, 100u32, 500u32] {
            let mut mode_flg = vec![0u32; w * h];
            // Use the GPU kernel for the mode_flg input (already byte-identical
            // to CPU per F-2A; this also exercises the chain).
            let st_m = unsafe {
                smooth_gpu_mode_flg_u8(
                    img.as_ptr(), mode_flg.as_mut_ptr(),
                    (w * 4) as i32, h as i32, range,
                )
            };
            assert_eq!(st_m, Status::Ok as u32);

            let mut dst_cpu: Vec<u32> = img.clone();
            cpu_link8_square_center_reference(&img, &mut dst_cpu, &mode_flg, w, h, range);

            let mut dst_gpu: Vec<u32> = img.clone();
            let st_g = unsafe {
                smooth_gpu_link8_square_center_u8(
                    img.as_ptr(),
                    dst_gpu.as_mut_ptr(),
                    mode_flg.as_ptr(),
                    (w * 4) as i32, h as i32, range,
                )
            };
            assert_eq!(st_g, Status::Ok as u32, "range={range}");
            for k in 0..(w * h) {
                if dst_cpu[k] != dst_gpu[k] {
                    panic!("link8_square_center pixel {k} differs (range={range}): cpu=0x{:08X} gpu=0x{:08X} src=0x{:08X} mode_flg={:#x}",
                           dst_cpu[k], dst_gpu[k], img[k], mode_flg[k]);
                }
            }
        }
    }

    // CPU reference for OFX-layout preprocess. Mirrors the AE-layout
    // reference but checks the right bits/bytes for OFX RGBA packing.
    fn cpu_preprocess_ofx_u8_reference(
        pixels: &mut [u32],
        width: usize,
        height: usize,
        is_white_trans: bool,
    ) -> SmoothBbox {
        let null: u32 = 0;
        let mut top: i32    = 0;
        let mut left: i32   = width as i32;
        let mut right: i32  = 0;
        let mut bottom: i32 = 0;
        let mut top_found  = false;
        let mut left_found = false;
        let mut t: usize = 0;

        let is_white   = |p: u32| -> bool { (p & 0x00FF_FFFF) == 0x00FF_FFFF };
        let alpha_zero = |p: u32| -> bool { ((p >> 24) & 0xFF) == 0 };

        if is_white_trans {
            for j in 0..height {
                if !top_found { top = j as i32; }
                for i in 0..width {
                    let p = pixels[t];
                    if is_white(p) {
                        pixels[t] = null;
                    } else if !alpha_zero(p) {
                        top_found = true; left_found = true;
                        let ii = i as i32; let jj = j as i32;
                        if left  > ii { left  = ii; }
                        if right < ii { right = ii; }
                        if bottom < jj { bottom = jj; }
                    }
                    t += 1;
                }
            }
        } else {
            for j in 0..height {
                if !top_found { top = j as i32; }
                for i in 0..width {
                    let p = pixels[t];
                    if !is_white(p) && !alpha_zero(p) {
                        top_found = true; left_found = true;
                        let ii = i as i32; let jj = j as i32;
                        if left  > ii { left  = ii; }
                        if right < ii { right = ii; }
                        if bottom < jj { bottom = jj; }
                    }
                    t += 1;
                }
            }
        }
        SmoothBbox {
            top:    if top_found  { top  } else { 0 },
            left:   if left_found { left } else { 0 },
            right:  right + 1,
            bottom: bottom + 1,
        }
    }

    fn pack_rgba_ofx(r: u8, g: u8, b: u8, a: u8) -> u32 {
        (r as u32) | ((g as u32) << 8) | ((b as u32) << 16) | ((a as u32) << 24)
    }

    #[test]
    fn preprocess_ofx_u8_matches_cpu_reference() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let w = 64usize; let h = 32usize;
        let mut img: Vec<u32> = Vec::with_capacity(w * h);
        for j in 0..h {
            for i in 0..w {
                let pixel = if (i + j) % 7 == 0 {
                    pack_rgba_ofx(0xFF, 0xFF, 0xFF, 0xFF)
                } else if (i + j) % 5 == 0 {
                    pack_rgba_ofx(0x00, 0x00, 0x00, 0x00)
                } else {
                    pack_rgba_ofx((i as u8) & 0xFF, (j as u8) & 0xFF, 0x40, 0xFF)
                };
                img.push(pixel);
            }
        }
        for &white_trans in &[false, true] {
            let mut cpu_img = img.clone();
            let cpu_bb = cpu_preprocess_ofx_u8_reference(&mut cpu_img, w, h, white_trans);

            let mut gpu_img = img.clone();
            let mut gpu_bb = SmoothBbox::default();
            let st = unsafe {
                smooth_gpu_preprocess_ofx_u8(
                    img.as_ptr(),
                    gpu_img.as_mut_ptr(),
                    (w * 4) as i32,
                    h as i32,
                    if white_trans { 1 } else { 0 },
                    &mut gpu_bb,
                )
            };
            assert_eq!(st, Status::Ok as u32, "white_trans={white_trans}");
            assert_eq!(cpu_bb, gpu_bb, "bbox mismatch (white_trans={white_trans})");
            for k in 0..(w * h) {
                if cpu_img[k] != gpu_img[k] {
                    panic!("OFX preprocess pixel {k} differs: cpu=0x{:08X} gpu=0x{:08X} (white_trans={white_trans})",
                           cpu_img[k], gpu_img[k]);
                }
            }
        }
    }

    /// All-transparent image must yield SmoothBbox {0, 0, 1, 1}.
    #[test]
    fn preprocess_u8_all_transparent() {
        if std::env::var("SMOOTH_GPU_SKIP_INIT_TEST").is_ok() { return; }
        let w = 4usize; let h = 3usize;
        let img: Vec<u32> = vec![0; w * h];
        let mut out: Vec<u32> = vec![0xDEADBEEFu32; w * h];
        let mut bb = SmoothBbox::default();
        let st = unsafe {
            smooth_gpu_preprocess_u8(
                img.as_ptr(), out.as_mut_ptr(),
                (w * 4) as i32, h as i32, 0, &mut bb,
            )
        };
        assert_eq!(st, Status::Ok as u32);
        assert_eq!(bb, SmoothBbox { top: 0, left: 0, right: 1, bottom: 1 });
        // Output must be all-transparent (matches CPU semantics: src copied through).
        assert!(out.iter().all(|&p| p == 0));
    }
}
