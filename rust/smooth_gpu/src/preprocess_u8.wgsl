// preprocess (8 bpc ARGB) compute shader.
//
// Pixel8 layout in memory: { alpha, red, green, blue } = bytes 0,1,2,3.
// When the host packs that as little-endian u32, the bit layout is
//   pixel u32 = (blue << 24) | (green << 16) | (red << 8) | alpha
// so:
//   alpha = pixel & 0xFF
//   red   = (pixel >>  8) & 0xFF
//   green = (pixel >> 16) & 0xFF
//   blue  = (pixel >> 24) & 0xFF
//
// White key: r == g == b == 0xFF (alpha is not part of the key).
// In packed form the white-key RGB bits are pixel >> 8 == 0xFFFFFF.
//
// Behaviour (matches smooth_core::pre_process for 8 bpc):
//   if is_white_trans != 0:
//     - if pixel is white-key (RGB == 0xFFFFFF): write null pixel (0x00000000)
//     - else if alpha == 0: leave dst untouched (caller pre-fills dst <- src)
//     - else: count in bbox + write src
//   else:
//     - if not white && alpha != 0: count in bbox + write src
//     - else: write src unchanged (no bbox update)
//
// bbox is a storage buffer of 4 atomic<i32>:
//   bbox[0] = top    (atomicMin), seeded to i32::MAX
//   bbox[1] = left   (atomicMin), seeded to i32::MAX
//   bbox[2] = right  (atomicMax), seeded to -1
//   bbox[3] = bottom (atomicMax), seeded to -1
// Caller computes the SmoothBbox-shaped result on CPU after readback:
//   if bbox[0] == i32::MAX => no pixels, return {0, 0, 1, 1}
//   else                   => {bbox[0], bbox[1], bbox[2]+1, bbox[3]+1}

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

    let alpha    = pixel & 0xFFu;
    let rgb_bits = (pixel >> 8u) & 0xFFFFFFu;
    let is_white = (rgb_bits == 0xFFFFFFu);
    let alpha_zero = (alpha == 0u);

    var out: u32 = pixel;
    var counts_in_bbox: bool = false;

    if (params.is_white_trans != 0u) {
        if (is_white) {
            // White-keyed transparent: write null pixel (RGBA = 0).
            out = 0u;
        } else if (!alpha_zero) {
            counts_in_bbox = true;
        }
        // else (alpha == 0, not white): skip bbox, write src unchanged.
    } else {
        // White stays as-is. Only non-white, non-zero-alpha pixels count.
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
