// mode_flg detection kernel (8 bpc).
//
// For each interior pixel (row in [1, height-1), col in [1, width-1)) compute
// the 4-bit mode_flg used by smooth_core::process_row_range as the dispatcher
// key for corner / link / lack / square handlers.
//
//   bit 0 = right  neighbour differs (delta_sum > range)
//   bit 1 = top    neighbour differs
//   bit 2 = bottom neighbour differs
//   bit 3 = left   neighbour differs
//
// Per smooth_core, the bits are only computed when
// fast_compare_pixel(self, right) is true (i.e. self != right at all). For
// pixels with self == right the row-loop short-circuits and mode_flg stays 0.
// We mirror that behaviour so the per-pixel result is byte-identical.
//
// Border pixels (i == 0 / i == width-1 / j == 0 / j == height-1) get
// mode_flg = 0 (the CPU side clamps the scan window to extent.{x1,y1}=1 and
// extent.{x2,y2}=width|height-1; this is the same effective behaviour).
//
// Output: u32 per pixel with mode_flg in the low 4 bits (28 bits unused).
// The verbose layout makes the byte-identity test trivial to write.

struct Params {
    width:  u32,
    height: u32,
    range:  u32,
    _pad:   u32,
};

@group(0) @binding(0) var<storage, read>       src:      array<u32>;
@group(0) @binding(1) var<storage, read_write> mode_out: array<u32>;
@group(0) @binding(2) var<uniform>             params:   Params;

// delta_sum for Pixel8 packed in u32 (LE: alpha=lo byte, then R, G, B).
fn delta_sum_u8(a: u32, b: u32) -> u32 {
    let a_a = a & 0xFFu;
    let a_r = (a >> 8u)  & 0xFFu;
    let a_g = (a >> 16u) & 0xFFu;
    let a_b = (a >> 24u) & 0xFFu;
    let b_a = b & 0xFFu;
    let b_r = (b >> 8u)  & 0xFFu;
    let b_g = (b >> 16u) & 0xFFu;
    let b_b = (b >> 24u) & 0xFFu;
    let d_a = select(b_a - a_a, a_a - b_a, a_a > b_a);
    let d_r = select(b_r - a_r, a_r - b_r, a_r > b_r);
    let d_g = select(b_g - a_g, a_g - b_g, a_g > b_g);
    let d_b = select(b_b - a_b, a_b - b_b, a_b > b_b);
    return d_a + d_r + d_g + d_b;
}

fn compare_pixel(a: u32, b: u32, range: u32) -> bool {
    return delta_sum_u8(a, b) > range;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let j = gid.y;
    let w = params.width;
    let h = params.height;
    if (i >= w || j >= h) { return; }

    let idx = j * w + i;
    var mode_flg: u32 = 0u;

    // Interior only; border pixels keep mode_flg = 0 just like the
    // smooth_core scan window does after the 1-pixel inward clamp.
    if (i >= 1u && i + 1u < w && j >= 1u && j + 1u < h) {
        let p_self  = src[idx];
        let p_right = src[idx + 1u];
        // fast_compare_pixel: any byte difference at all (cheap u32 != u32).
        if (p_self != p_right) {
            let p_left = src[idx - 1u];
            let p_top  = src[idx - w];
            let p_bot  = src[idx + w];
            if (compare_pixel(p_self, p_right, params.range)) { mode_flg |= 1u << 0u; }
            if (compare_pixel(p_self, p_top,   params.range)) { mode_flg |= 1u << 1u; }
            if (compare_pixel(p_self, p_bot,   params.range)) { mode_flg |= 1u << 2u; }
            if (compare_pixel(p_self, p_left,  params.range)) { mode_flg |= 1u << 3u; }
        }
    }

    mode_out[idx] = mode_flg;
}
