// Passthrough compute shader.
// Copies u32 elements from src -> dst, one per invocation.
// Used by Phase 3-B as the simplest possible "is the GPU pipeline alive" test;
// also the foundation that the real preprocess / scan-blend kernels will build on.

@group(0) @binding(0) var<storage, read>       src: array<u32>;
@group(0) @binding(1) var<storage, read_write> dst: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= arrayLength(&src)) { return; }
    dst[i] = src[i];
}
