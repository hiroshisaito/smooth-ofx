// preprocess (8 bpc OFX RGBA) compute shader.
//
// OFX OfxRGBAColourB layout: { r, g, b, a } = bytes 0,1,2,3.
// As little-endian u32: bits[0..24] = R/G/B, bits[24..32] = alpha.
//   alpha = (pixel >> 24) & 0xFF
//   r = pixel & 0xFF, g = (pixel >> 8) & 0xFF, b = (pixel >> 16) & 0xFF
//
// White key: r == g == b == 0xFF (alpha is not part of the key).
// In packed form: (pixel & 0xFFFFFF) == 0xFFFFFF.
//
// This is the OFX-native counterpart of preprocess_u8.wgsl (which uses
// AE's ARGB layout). Having a dedicated kernel avoids two extra full-
// buffer swizzle passes in the render path, preserving the speed-up
// preprocess on GPU is meant to deliver.
//
// Behaviour (matches smooth.cpp::preProcess<OfxRGBAColourB> + isWhitePixel):
//   if is_white_trans != 0:
//     - if pixel is white-key (RGB == 0xFFFFFF): write null pixel (0)
//     - else if alpha == 0: leave dst as-is (caller pre-fills dst <- src)
//     - else: count in bbox + write src
//   else:
//     - if not white && alpha != 0: count in bbox + write src
//     - else: write src unchanged (no bbox update)
//
// bbox is `array<atomic<i32>, 4>`:
//   [top]    atomicMin, seeded i32::MAX
//   [left]   atomicMin, seeded i32::MAX
//   [right]  atomicMax, seeded -1
//   [bottom] atomicMax, seeded -1
// Caller projects raw atomic results onto the smooth_core SmoothBbox shape
// (right += 1, bottom += 1; if no pixels found, return {0, 0, 1, 1}).

struct Params {
    width:           u32,
    height:          u32,
    is_white_trans:  u32,
    _pad:            u32,
};

@group(0) @binding(0) var<storage, read>       src:    array<u32>;
@group(0) @binding(1) var<storage, read_write> dst:    array<u32>;
@group(0) @binding(2) var<storage, read_write> bbox:   array<atomic<i32>, 4>;
@group(0) @binding(3) var<uniform>             params: Params;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let j = gid.y;
    if (i >= params.width || j >= params.height) {
        return;
    }
    let idx = j * params.width + i;
    let pixel = src[idx];

    let alpha    = (pixel >> 24u) & 0xFFu;
    let rgb_bits = pixel & 0xFFFFFFu;
    let is_white = (rgb_bits == 0xFFFFFFu);
    let alpha_zero = (alpha == 0u);

    var out: u32 = pixel;
    var counts_in_bbox: bool = false;

    if (params.is_white_trans != 0u) {
        if (is_white) {
            out = 0u;
        } else if (!alpha_zero) {
            counts_in_bbox = true;
        }
    } else {
        if (!is_white && !alpha_zero) {
            counts_in_bbox = true;
        }
    }

    dst[idx] = out;

    if (counts_in_bbox) {
        atomicMin(&bbox[0], i32(j));
        atomicMin(&bbox[1], i32(i));
        atomicMax(&bbox[2], i32(i));
        atomicMax(&bbox[3], i32(j));
    }
}
