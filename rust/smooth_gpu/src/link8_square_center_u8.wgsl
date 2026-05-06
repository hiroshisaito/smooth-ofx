// link8_square center handler (8 bpc).
//
// Ports the CENTER part of smooth_core::link8_square_execute (lines 416-446
// of smooth-ae/rust/smooth_core/src/link8.rs). The OUTSIDE expansion
// (link8_square_blend_outside) is intentionally deferred to a later
// sub-phase — it requires count_length_two_lines + blend_line which run
// sequentially along a row/column.
//
// Operates per-pixel: only writes dst[idx] when mode_flg[idx] == 15
// (i.e. the pixel is the center of a "square" pattern, which is the
// dispatch case for smooth_core's link8_square_execute). The kernel is
// safe to dispatch over the full image; non-15 pixels are left untouched
// (caller must pre-initialise dst to src).
//
// Algorithm:
//   1. Compute 4 diagonal compare-equal flags vs (TL, TR, BR, BL).
//   2. For each diagonal i:
//        if equal: temp[i] = self
//        else:     temp[i] = blending_pixel_f(self, neighbour, 0.5)
//   3. Output pixel = average of the 4 temps.
//
// blending_pixel_f for ratio=0.5 with max_value=0xFF on Pixel8 (8bpc):
//   alpha_v = (0xFF * 0.5) truncated = 0x7F
//   r_alpha = max - alpha_v = 0x80
//   if both inputs alpha == 0xFF: out.alpha = 0xFF, RGB blended.
//   if target.alpha == 0:         alpha blended, RGB = ref.
//   if ref.alpha    == 0:         alpha blended, RGB = target.
//   else:                         all 4 channels blended.

struct Params {
    width:  u32,
    height: u32,
    range:  u32,
    _pad:   u32,
};

@group(0) @binding(0) var<storage, read>       src:      array<u32>;
@group(0) @binding(1) var<storage, read_write> dst:      array<u32>;
@group(0) @binding(2) var<storage, read>       mode_flg: array<u32>;
@group(0) @binding(3) var<uniform>             params:   Params;

fn unpack(p: u32) -> vec4<u32> {
    return vec4<u32>(p & 0xFFu, (p >> 8u) & 0xFFu, (p >> 16u) & 0xFFu, (p >> 24u) & 0xFFu);
}
fn pack(v: vec4<u32>) -> u32 {
    return (v.x & 0xFFu) | ((v.y & 0xFFu) << 8u) | ((v.z & 0xFFu) << 16u) | ((v.w & 0xFFu) << 24u);
}

fn delta_sum(a: u32, b: u32) -> u32 {
    let aa = unpack(a); let bb = unpack(b);
    let d = vec4<u32>(
        select(bb.x - aa.x, aa.x - bb.x, aa.x > bb.x),
        select(bb.y - aa.y, aa.y - bb.y, aa.y > bb.y),
        select(bb.z - aa.z, aa.z - bb.z, aa.z > bb.z),
        select(bb.w - aa.w, aa.w - bb.w, aa.w > bb.w),
    );
    return d.x + d.y + d.z + d.w;
}

/// blending_pixel_f for ratio=0.5, u8 ARGB.
fn blend_half(tgt: u32, refp: u32) -> u32 {
    let max_v: u32  = 0xFFu;
    let a_v:   u32  = 0x7Fu;
    let r_v:   u32  = 0x80u;
    let t = unpack(tgt);
    let r = unpack(refp);
    let tp_a = t.x;
    let rp_a = r.x;
    var out: vec4<u32>;
    if (tp_a == max_v && rp_a == max_v) {
        out = vec4<u32>(
            max_v,
            (t.y * a_v + r.y * r_v) / max_v,
            (t.z * a_v + r.z * r_v) / max_v,
            (t.w * a_v + r.w * r_v) / max_v,
        );
    } else if (tp_a == 0u) {
        out = vec4<u32>((tp_a * a_v + rp_a * r_v) / max_v, r.y, r.z, r.w);
    } else if (rp_a == 0u) {
        out = vec4<u32>((tp_a * a_v + rp_a * r_v) / max_v, t.y, t.z, t.w);
    } else {
        out = vec4<u32>(
            (tp_a * a_v + rp_a * r_v) / max_v,
            (t.y  * a_v + r.y  * r_v) / max_v,
            (t.z  * a_v + r.z  * r_v) / max_v,
            (t.w  * a_v + r.w  * r_v) / max_v,
        );
    }
    return pack(out);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let j = gid.y;
    let w = params.width;
    let h = params.height;
    if (i >= w || j >= h) { return; }
    let idx = j * w + i;

    // Only handle pixels dispatched to link8_square (mode_flg == 15).
    if ((mode_flg[idx] & 0xFu) != 15u) { return; }

    // mode_flg_u8 already returns 0 on the 1-pixel border, so any pixel
    // with mode_flg == 15 has interior diagonal neighbours.
    let p_self = src[idx];
    let p_tl   = src[idx - w - 1u];
    let p_tr   = src[idx - w + 1u];
    let p_br   = src[idx + w + 1u];
    let p_bl   = src[idx + w - 1u];

    let r = params.range;
    let eq_tl = delta_sum(p_self, p_tl) <= r;
    let eq_tr = delta_sum(p_self, p_tr) <= r;
    let eq_br = delta_sum(p_self, p_br) <= r;
    let eq_bl = delta_sum(p_self, p_bl) <= r;

    let t0 = select(blend_half(p_self, p_tl), p_self, eq_tl);
    let t1 = select(blend_half(p_self, p_tr), p_self, eq_tr);
    let t2 = select(blend_half(p_self, p_br), p_self, eq_br);
    let t3 = select(blend_half(p_self, p_bl), p_self, eq_bl);

    let u0 = unpack(t0);
    let u1 = unpack(t1);
    let u2 = unpack(t2);
    let u3 = unpack(t3);
    let sum = u0 + u1 + u2 + u3;
    // Integer div by 4 (floor) — matches smooth_core's div_by_int(4) for u32.
    let avg = vec4<u32>(sum.x >> 2u, sum.y >> 2u, sum.z >> 2u, sum.w >> 2u);

    dst[idx] = pack(avg);
}
