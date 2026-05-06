//
// smooth.cpp - OFX entry point
// Port of smooth-ae (After Effects plugin) to OpenFX 1.5.1
//
// Step 3: describe / describeInContext / createInstance / destroyInstance
// Step 4: render() — 8bpc + 16bpc, tight-buffer copy + smoothing algorithm
//

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <new>
#include <type_traits>

#ifndef SMOOTH_OFX_VERSION
#  define SMOOTH_OFX_VERSION "1.4.0"
#endif
#ifndef SMOOTH_OFX_GIT_SHA
#  define SMOOTH_OFX_GIT_SHA "unknown"
#endif

#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxPixels.h"

#include "define.h"
#include "util.h"
#include "upMode.h"
#include "downMode.h"
#include "8link.h"
#include "Lack.h"

#ifdef SMOOTH_OFX_USE_RUST_CORE
extern "C" {
#include "smooth_core_ffi.h"
}
#endif

#ifdef SMOOTH_OFX_USE_GPU_CORE
extern "C" {
#include "smooth_gpu_ffi.h"
}
#endif

//---------------------------------------------------------------------------//
// ホスト情報
//---------------------------------------------------------------------------//
static OfxHost                *gHost        = nullptr;
static OfxImageEffectSuiteV1  *gEffectHost  = nullptr;
static OfxPropertySuiteV1     *gPropHost    = nullptr;
static OfxParameterSuiteV1    *gParamHost   = nullptr;
static OfxMemorySuiteV1       *gMemoryHost  = nullptr;
static OfxMultiThreadSuiteV1  *gThreadHost  = nullptr;

//---------------------------------------------------------------------------//
// パラメータ名
//---------------------------------------------------------------------------//
#define kParamWhiteOption   "whiteOption"
#define kParamRange         "range"
#define kParamLineWeight    "lineWeight"
#define kParamUseGpu        "useGpu"
#define kParamBuildInfo     "buildInfo"

//---------------------------------------------------------------------------//
// インスタンスごとに保持するデータ
//---------------------------------------------------------------------------//
struct MyInstanceData {
    OfxImageClipHandle  sourceClip;
    OfxImageClipHandle  outputClip;
    OfxParamHandle      whiteOptionParam;
    OfxParamHandle      rangeParam;
    OfxParamHandle      lineWeightParam;
#ifdef SMOOTH_OFX_USE_GPU_CORE
    OfxParamHandle      useGpuParam;
#endif
    OfxParamHandle      buildInfoParam;
};

static MyInstanceData *
getMyInstanceData(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);

    MyInstanceData *myData = nullptr;
    gPropHost->propGetPointer(effectProps, kOfxPropInstanceData, 0, (void **)&myData);
    return myData;
}

//---------------------------------------------------------------------------//
// ピクセルの白/透明ヘルパー
//---------------------------------------------------------------------------//
template <typename PixelType>
static inline void getWhitePixel(PixelType *p)
{
    const unsigned int m = getMaxValue<PixelType>();
    p->r = (decltype(p->r))m;
    p->g = (decltype(p->g))m;
    p->b = (decltype(p->b))m;
    p->a = (decltype(p->a))m;
}

template <typename PixelType>
static inline void getNullPixel(PixelType *p)
{
    p->r = 0;
    p->g = 0;
    p->b = 0;
    p->a = 0;
}

// プリマルチプライド RGBA → ストレート RGBA に展開 (in-place)
template <typename PixelType>
static inline void unpremultBuffer(PixelType *buf, int count)
{
    const unsigned int m = getMaxValue<PixelType>();
    for (int i = 0; i < count; i++) {
        unsigned int a = buf[i].a;
        if (a == 0 || a == m) continue;
        unsigned int r = ((unsigned int)buf[i].r * m + a / 2) / a;
        unsigned int g = ((unsigned int)buf[i].g * m + a / 2) / a;
        unsigned int b = ((unsigned int)buf[i].b * m + a / 2) / a;
        if (r > m) r = m;
        if (g > m) g = m;
        if (b > m) b = m;
        buf[i].r = (decltype(buf[i].r))r;
        buf[i].g = (decltype(buf[i].g))g;
        buf[i].b = (decltype(buf[i].b))b;
    }
}

template <>
inline void unpremultBuffer<OfxRGBAColourF>(OfxRGBAColourF *buf, int count)
{
    for (int i = 0; i < count; i++) {
        float a = buf[i].a;
        if (a <= 0.0f || a >= 1.0f) continue;
        buf[i].r = buf[i].r / a;
        buf[i].g = buf[i].g / a;
        buf[i].b = buf[i].b / a;
    }
}

// ストレート RGBA → プリマルチプライド RGBA に畳み込み (in-place)
template <typename PixelType>
static inline void premultBuffer(PixelType *buf, int count)
{
    const unsigned int m = getMaxValue<PixelType>();
    for (int i = 0; i < count; i++) {
        unsigned int a = buf[i].a;
        if (a == m) continue;
        if (a == 0) { buf[i].r = 0; buf[i].g = 0; buf[i].b = 0; continue; }
        buf[i].r = (decltype(buf[i].r))(((unsigned int)buf[i].r * a + m / 2) / m);
        buf[i].g = (decltype(buf[i].g))(((unsigned int)buf[i].g * a + m / 2) / m);
        buf[i].b = (decltype(buf[i].b))(((unsigned int)buf[i].b * a + m / 2) / m);
    }
}

template <>
inline void premultBuffer<OfxRGBAColourF>(OfxRGBAColourF *buf, int count)
{
    for (int i = 0; i < count; i++) {
        float a = buf[i].a;
        if (a >= 1.0f) continue;
        if (a <= 0.0f) { buf[i].r = 0.0f; buf[i].g = 0.0f; buf[i].b = 0.0f; continue; }
        buf[i].r = buf[i].r * a;
        buf[i].g = buf[i].g * a;
        buf[i].b = buf[i].b * a;
    }
}

//---------------------------------------------------------------------------//
// isWhitePixel: ピクセルが「白」とみなせるかの判定。ホスト側 (DaVinci Resolve)
// が 16bpc / float で ピュアな 0xFFFF / 1.0 ではない値を「白」として渡してくる
// ケースに対応するため、type 別に許容差を持たせる。
//
// - 8bpc:  厳密一致 (0xFF を期待。Resolve も 0xFF で渡してくる)
// - 16bpc: 0xFFFF (OFX 1.5.1 標準) と 0x8000 (AE 流の half-range 規約) の両方を
//          許容。多少のドリフト (±0x0100) も吸収
// - float: 1.0 ± 0.005 程度の許容差で判定 (色管理によるわずかなずれを吸収)
//---------------------------------------------------------------------------//
template <typename PixelType>
static inline bool isWhitePixel(const PixelType &p);

template <> inline bool isWhitePixel<OfxRGBAColourB>(const OfxRGBAColourB &p) {
    return p.r == 0xFF && p.g == 0xFF && p.b == 0xFF;
}
template <> inline bool isWhitePixel<OfxRGBAColourS>(const OfxRGBAColourS &p) {
    auto near = [](unsigned int target, unsigned int v, unsigned int tol) {
        return (v + tol >= target) && (v <= target + tol);
    };
    constexpr unsigned int TOL = 0x0100;
    bool near_full = near(0xFFFF, p.r, TOL) && near(0xFFFF, p.g, TOL) && near(0xFFFF, p.b, TOL);
    bool near_half = near(0x8000, p.r, TOL) && near(0x8000, p.g, TOL) && near(0x8000, p.b, TOL);
    return near_full || near_half;
}
template <> inline bool isWhitePixel<OfxRGBAColourF>(const OfxRGBAColourF &p) {
    constexpr float EPS = 0.005f;
    return  p.r > 1.0f - EPS && p.r < 1.0f + EPS
         && p.g > 1.0f - EPS && p.g < 1.0f + EPS
         && p.b > 1.0f - EPS && p.b < 1.0f + EPS;
}

//---------------------------------------------------------------------------//
// preProcess: 白抜き + 処理領域（extent）の検出
//   in_ptr はタイトに詰められた width*height のバッファを想定
//---------------------------------------------------------------------------//
template <typename PixelType>
static void preProcess(PixelType *in_ptr, int width, int height, OfxRectI *rect, bool is_white_trans)
{
    PixelType null_pixel;
    getNullPixel(&null_pixel);

    int top = 0, left = width, right = 0, bottom = 0;
    bool top_found = false, left_found = false;

    int t = 0;
    for (int j = 0; j < height; j++)
    {
        if (!top_found) top = j;

        for (int i = 0; i < width; i++, t++)
        {
            const bool is_white = isWhitePixel<PixelType>(in_ptr[t]);

            if (is_white_trans && is_white)
            {
                in_ptr[t] = null_pixel;
                continue;
            }

            if (in_ptr[t].a == 0)
                continue;

            if (!is_white_trans && is_white)
            {
                continue;
            }

            top_found = true;
            left_found = true;
            if (left > i)   left = i;
            if (right < i)  right = i;
            if (bottom < j) bottom = j;
        }
    }

    rect->x1 = left_found ? left : 0;
    rect->y1 = top_found  ? top  : 0;
    rect->x2 = right + 1;
    rect->y2 = bottom + 1;
}

//---------------------------------------------------------------------------//
// smoothing: メインアルゴリズム
//   in_ptr / out_ptr はタイト (stride == width) なバッファ
//   in_ptr は破壊的に変更される (preProcess)
//---------------------------------------------------------------------------//
template <typename PixelType>
static void smoothing(PixelType *in_ptr,
                      PixelType *out_ptr,
                      int width,
                      int height,
                      bool white_option,
                      double range_param,
                      double line_weight_param,
                      bool use_gpu)
{
    // 白抜き + 領域検出
    OfxRectI extent;

#ifdef SMOOTH_OFX_USE_GPU_CORE
    // ハイブリッド経路 (Phase F-4): 8bpc は GPU preprocess に投げる。
    // OFX RGBA layout 専用 kernel `smooth_gpu_preprocess_ofx_u8` を使うので
    // swizzle 不要、in-place で white-key 抜きと bbox 取得が GPU 並列で行える。
    // GPU init / dispatch が失敗した場合は黙って CPU preProcess に fallback。
    // `use_gpu` (Inspector の "GPU" チェックボックス) で完全無効化も可能。
    bool gpu_preprocess_ok = false;
    if (use_gpu) {
        if constexpr (std::is_same_v<PixelType, OfxRGBAColourB>) {
            smooth_gpu_bbox_t gpu_bbox;
            const int32_t rowbytes = width * (int32_t)sizeof(PixelType);
            const uint32_t st = smooth_gpu_preprocess_ofx_u8(
                (const uint32_t *)in_ptr, (uint32_t *)in_ptr,
                rowbytes, height, white_option ? 1 : 0, &gpu_bbox);
            if (st == SMOOTH_GPU_STATUS_OK) {
                extent.x1 = gpu_bbox.left;
                extent.y1 = gpu_bbox.top;
                extent.x2 = gpu_bbox.right;
                extent.y2 = gpu_bbox.bottom;
                gpu_preprocess_ok = true;
            }
        }
    }
    if (!gpu_preprocess_ok) {
        preProcess<PixelType>(in_ptr, width, height, &extent, white_option);
    }
#else
    (void)use_gpu;  // unused when smooth_gpu is not linked
    preProcess<PixelType>(in_ptr, width, height, &extent, white_option);
#endif

    // 入力を出力にコピー
    std::memcpy(out_ptr, in_ptr, sizeof(PixelType) * (size_t)width * (size_t)height);

    // range の型はピクセル種別で変える (整数: unsigned int、float: float)
    // スケール: range_param (0..100) を 4 channel 分の色距離スケールへ変換
    typename PixelRangeType<PixelType>::type range =
        (typename PixelRangeType<PixelType>::type)(range_param *
        (getMaxValue<PixelType>() * 4) / 100.0);
    float line_weight  = (float)(line_weight_param / 2.0 + 0.5);
    float weight;
    bool  lack_flg;

    BlendingInfo<PixelType> blend_info;
    BlendingInfo<PixelType> *info = &blend_info;

    blend_info.in_ptr     = in_ptr;
    blend_info.out_ptr    = out_ptr;
    blend_info.in_stride  = width;
    blend_info.out_stride = width;
    blend_info.height     = height;
    blend_info.range      = range;
    blend_info.LineWeight = line_weight;

    // 領域を 1px 内側にクリップ (端で近傍参照が走らないように)
    if (extent.y1 == 0)        extent.y1 = 1;
    if (extent.x1 == 0)        extent.x1 = 1;
    if (extent.x2 == width)    extent.x2 -= 1;
    if (extent.y2 == height)   extent.y2 -= 1;

    if (extent.x2 <= extent.x1 || extent.y2 <= extent.y1)
        return;  // 処理対象なし

    const int in_width = width;

    for (int j = extent.y1; j < extent.y2; j++)
    {
        lack_flg = false;

        long in_target  = (long)j * in_width + extent.x1;
        long out_target = (long)j * in_width + extent.x1;

        for (int i = extent.x1; i < extent.x2; i++, in_target++, out_target++)
        {
            // 直前のアンチ処理で「欠け」の可能性が立ったときの補助処理
            if (lack_flg)
            {
                lack_flg = false;
                blend_info.i          = i;
                blend_info.j          = j;
                blend_info.in_target  = in_target;
                blend_info.out_target = out_target;
                blend_info.flag       = 0;
                LackMode0304Execute(&blend_info);
            }

            // 隣接ピクセルと色が違うピクセルだけを角候補として処理
            if (ComparePixel(in_target, in_target + 1))
            {
                unsigned char mode_flg = 0;

                blend_info.i          = i;
                blend_info.j          = j;
                blend_info.in_target  = in_target;
                blend_info.out_target = out_target;
                blend_info.flag       = 0;
                std::memset(&blend_info.core, 0, sizeof(Cinfo) * 4);

                if (ComparePixel(in_target, in_target + 1))          mode_flg |= 1 << 0;
                if (ComparePixel(in_target, in_target - in_width))   mode_flg |= 1 << 1;
                if (ComparePixel(in_target, in_target + in_width))   mode_flg |= 1 << 2;
                if (ComparePixel(in_target, in_target - 1))          mode_flg |= 1 << 3;

                if (mode_flg != 0)
                {
                    if (i < in_width - 2 && (mode_flg & (1 << 0)))
                        lack_flg = true;

                    switch (mode_flg)
                    {
                    case 3: // 上向きの角
                        if (ComparePixelEqual(in_target - in_width,     in_target + 1) &&
                            ComparePixel     (in_target - in_width + 1, in_target - in_width) &&
                            ComparePixel     (in_target - in_width + 1, in_target + 1))
                        {
                            break;
                        }

                        upMode_LeftCountLength  <PixelType>(&blend_info);
                        upMode_RightCountLength <PixelType>(&blend_info);
                        upMode_TopCountLength   <PixelType>(&blend_info);
                        upMode_BottomCountLength<PixelType>(&blend_info);

                        // 水平方向補正
                        if (blend_info.core[0].length - blend_info.core[1].length == 1)
                        {
                            blend_info.core[0].start -= 0.5f;
                            blend_info.core[1].start -= 0.5f;
                        }
                        weight = ((blend_info.core[0].flg & CR_FLG_FILL) ||
                                  (blend_info.core[1].flg & CR_FLG_FILL))
                                 ? 0.5f : blend_info.LineWeight;
                        blend_info.core[0].end = blend_info.core[0].start - (blend_info.core[0].start - blend_info.core[0].end) * weight;
                        blend_info.core[1].end = blend_info.core[1].start + (blend_info.core[1].end   - blend_info.core[1].start) * weight;

                        // 垂直方向補正
                        if (blend_info.core[3].length - blend_info.core[2].length == 1)
                        {
                            blend_info.core[2].start += 0.5f;
                            blend_info.core[3].start += 0.5f;
                        }
                        weight = ((blend_info.core[2].flg & CR_FLG_FILL) ||
                                  (blend_info.core[3].flg & CR_FLG_FILL))
                                 ? 0.5f : blend_info.LineWeight;
                        blend_info.core[2].end = blend_info.core[2].start - (blend_info.core[2].start - blend_info.core[2].end) * weight;
                        blend_info.core[3].end = blend_info.core[3].start + (blend_info.core[3].end   - blend_info.core[3].start) * weight;

                        if (blend_info.core[0].length >= 2 && blend_info.core[3].length >= 2)
                        {
                            LackMode02Execute(&blend_info);
                        }
                        else if (blend_info.core[1].length > 0)
                        {
                            blend_info.mode = BLEND_MODE_UP_H;
                            upMode_LeftBlending <PixelType>(&blend_info);
                            upMode_RightBlending<PixelType>(&blend_info);
                            if (blend_info.core[2].length > 1)
                            {
                                upMode_TopBlending   <PixelType>(&blend_info);
                                upMode_BottomBlending<PixelType>(&blend_info);
                            }
                        }
                        else if (blend_info.core[2].length > 0)
                        {
                            blend_info.mode = BLEND_MODE_UP_V;
                            upMode_TopBlending   <PixelType>(&blend_info);
                            upMode_BottomBlending<PixelType>(&blend_info);
                        }
                        break;

                    case 5: // 下向きの角
                        if (ComparePixelEqual(in_target + in_width,     in_target + 1) &&
                            ComparePixel     (in_target + in_width + 1, in_target + in_width) &&
                            ComparePixel     (in_target + in_width + 1, in_target + 1))
                        {
                            break;
                        }

                        downMode_LeftCountLength  <PixelType>(&blend_info);
                        downMode_RightCountLength <PixelType>(&blend_info);
                        downMode_TopCountLength   <PixelType>(&blend_info);
                        downMode_BottomCountLength<PixelType>(&blend_info);

                        if (blend_info.core[0].length - blend_info.core[1].length == 1)
                        {
                            blend_info.core[0].start -= 0.5f;
                            blend_info.core[1].start -= 0.5f;
                        }
                        weight = ((blend_info.core[0].flg & CR_FLG_FILL) ||
                                  (blend_info.core[1].flg & CR_FLG_FILL))
                                 ? 0.5f : blend_info.LineWeight;
                        blend_info.core[0].end = blend_info.core[0].start - (blend_info.core[0].start - blend_info.core[0].end) * weight;
                        blend_info.core[1].end = blend_info.core[1].start + (blend_info.core[1].end   - blend_info.core[1].start) * weight;

                        if (blend_info.core[3].length - blend_info.core[2].length == 1)
                        {
                            blend_info.core[2].start += 0.5f;
                            blend_info.core[3].start += 0.5f;
                        }
                        weight = ((blend_info.core[2].flg & CR_FLG_FILL) ||
                                  (blend_info.core[3].flg & CR_FLG_FILL))
                                 ? 0.5f : blend_info.LineWeight;
                        blend_info.core[2].end = blend_info.core[2].start - (blend_info.core[2].start - blend_info.core[2].end) * weight;
                        blend_info.core[3].end = blend_info.core[3].start + (blend_info.core[3].end   - blend_info.core[3].start) * weight;

                        if (blend_info.core[0].length >= 2 && blend_info.core[2].length >= 2)
                        {
                            LackMode01Execute(&blend_info);
                        }
                        else if (blend_info.core[1].length > 0)
                        {
                            blend_info.mode = BLEND_MODE_UP_H;
                            downMode_LeftBlending <PixelType>(&blend_info);
                            downMode_RightBlending<PixelType>(&blend_info);
                            if (blend_info.core[3].length > 1)
                            {
                                downMode_TopBlending   <PixelType>(&blend_info);
                                downMode_BottomBlending<PixelType>(&blend_info);
                            }
                        }
                        else if (blend_info.core[3].length > 0)
                        {
                            blend_info.mode = BLEND_MODE_UP_V;
                            downMode_TopBlending   <PixelType>(&blend_info);
                            downMode_BottomBlending<PixelType>(&blend_info);
                        }
                        break;

                    case 7:  Link8Mode01Execute<PixelType>(&blend_info); break;  // 突起 コ
                    case 11: Link8Mode02Execute<PixelType>(&blend_info); break;  // 突起 п
                    case 13: Link8Mode04Execute<PixelType>(&blend_info); break;  // 突起 ш
                    case 15: Link8SquareExecute <PixelType>(&blend_info); break; // 四角
                    default: break;
                    }

                    // 突起 mode3 (ヒ) の 2nd-pass
                    if (i < in_width - 2)
                    {
                        blend_info.i          = i + 1;
                        blend_info.j          = j;
                        blend_info.in_target  = in_target + 1;
                        blend_info.out_target = out_target + 1;
                        blend_info.flag       = 0;

                        unsigned char m2 = 0;
                        if (ComparePixel(blend_info.in_target, blend_info.in_target - in_width))  m2 |= 1 << 0;
                        if (ComparePixel(blend_info.in_target, blend_info.in_target + in_width))  m2 |= 1 << 1;
                        if (ComparePixel(blend_info.in_target, blend_info.in_target + 1))         m2 |= 1 << 2;

                        if (m2 == 3)
                            Link8Mode03Execute<PixelType>(&blend_info);
                    }
                }
            }
        }
    }
}

//---------------------------------------------------------------------------//
// ロード / アンロード
//---------------------------------------------------------------------------//
static OfxStatus
onLoad(void)
{
    if (!gHost) return kOfxStatErrMissingHostFeature;

    gEffectHost  = (OfxImageEffectSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gPropHost    = (OfxPropertySuiteV1 *)    gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gParamHost   = (OfxParameterSuiteV1 *)   gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gMemoryHost  = (OfxMemorySuiteV1 *)      gHost->fetchSuite(gHost->host, kOfxMemorySuite, 1);
    gThreadHost  = (OfxMultiThreadSuiteV1 *) gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);

    if (!gEffectHost || !gPropHost || !gParamHost)
        return kOfxStatErrMissingHostFeature;

    return kOfxStatOK;
}

static OfxStatus
onUnload(void)
{
    return kOfxStatOK;
}

//---------------------------------------------------------------------------//
// describe
//---------------------------------------------------------------------------//
static OfxStatus
describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);

    gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Smooth");
    gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Filter");

    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);

    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, 0);
    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gPropHost->propSetString(effectProps, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);

    return kOfxStatOK;
}

//---------------------------------------------------------------------------//
// describeInContext
//---------------------------------------------------------------------------//
static OfxStatus
describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/)
{
    OfxPropertySetHandle props;

    gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);

    gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);

    OfxParamSetHandle paramSet;
    gEffectHost->getParamSet(effect, &paramSet);

    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamWhiteOption, &props);
    gPropHost->propSetInt(props, kOfxParamPropDefault, 0, 0);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "transparent");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Treat white as transparent (alpha=0 areas are skipped when off)");
    gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamWhiteOption);

    gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParamRange, &props);
    gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
    gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.0);
    gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 100.0);
    gPropHost->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0.0);
    gPropHost->propSetDouble(props, kOfxParamPropDisplayMax, 0, 100.0);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "range");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Color distance threshold for treating neighboring pixels as equal (0=tight, 100=disable smoothing)");
    gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamRange);

    gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, kParamLineWeight, &props);
    gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 0.0);
    gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.0);
    gPropHost->propSetDouble(props, kOfxParamPropMax, 0, 1.0);
    gPropHost->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0.0);
    gPropHost->propSetDouble(props, kOfxParamPropDisplayMax, 0, 1.0);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "line weight");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Smoothing line weight (0=thin, 1=thick)");
    gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamLineWeight);

#ifdef SMOOTH_OFX_USE_GPU_CORE
    // GPU acceleration toggle. Only present in builds that link smooth_gpu.
    // Default ON: when GPU init succeeds, the 8bpc preprocess pass runs on
    // GPU. Toggle OFF to force the C++ preprocess fallback (useful for A/B
    // verification, or for the case where the GPU round-trip is slower than
    // the host's CPU preprocess on the current image / hardware).
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamUseGpu, &props);
    gPropHost->propSetInt(props, kOfxParamPropDefault, 0, 1);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "GPU");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Use GPU acceleration for the 8bpc preprocess pass. May be slower than CPU on this build because the GPU round-trip cost can outweigh the savings on small kernels.");
    gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamUseGpu);
#endif

    // ビルド ID 表示用 read-only テキストフィールド。
    // DaVinci Resolve は `kOfxParamStringIsLabel` の値をレンダリングしないため、
    // 「SingleLine + Enabled=0 (disabled)」という他 OFX プラグインでも一般的な
    // パターンに切り替え、通常の text widget をグレーアウト表示で読み取り専用にする。
    {
        char buildIdStr[320];
        char *cur = buildIdStr;
        char *end = buildIdStr + sizeof(buildIdStr);
        int n;
#ifdef SMOOTH_OFX_USE_RUST_CORE
        const char *coreId = smooth_core_build_id();
        n = std::snprintf(cur, end - cur, "%s+%s / rust core %s",
                          SMOOTH_OFX_VERSION, SMOOTH_OFX_GIT_SHA,
                          (coreId && *coreId) ? coreId : "?");
#else
        n = std::snprintf(cur, end - cur, "%s+%s / cpp core",
                          SMOOTH_OFX_VERSION, SMOOTH_OFX_GIT_SHA);
#endif
        if (n > 0 && cur + n < end) cur += n;

#ifdef SMOOTH_OFX_USE_GPU_CORE
        // smooth_gpu_init() がここで呼ばれることで staticlib のシンボルが retain
        // される (DCE 解除)。GPU を実際にレンダ経路で使うのは Phase F 以降。
        // ここでは init 結果と build id を Inspector の build ラベルに出すだけ。
        const uint32_t gpuStatus = smooth_gpu_init();
        const char *gpuId = smooth_gpu_build_id();
        const char *gpuStatusStr =
            (gpuStatus == SMOOTH_GPU_STATUS_OK)                 ? "ok"        :
            (gpuStatus == SMOOTH_GPU_STATUS_NO_ADAPTER)         ? "no-adapter":
            (gpuStatus == SMOOTH_GPU_STATUS_DEVICE_CREATE_FAIL) ? "no-device" :
                                                                  "fail";
        n = std::snprintf(cur, end - cur, " | gpu %s %s",
                          gpuStatusStr, (gpuId && *gpuId) ? gpuId : "?");
        if (n > 0 && cur + n < end) cur += n;
#endif
        gParamHost->paramDefine(paramSet, kOfxParamTypeString, kParamBuildInfo, &props);
        gPropHost->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
        gPropHost->propSetString(props, kOfxParamPropDefault, 0, buildIdStr);
        gPropHost->propSetString(props, kOfxPropLabel, 0, "build");
        gPropHost->propSetString(props, kOfxParamPropHint, 0, "Plugin build identity (read-only).");
        gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamBuildInfo);
        gPropHost->propSetInt(props, kOfxParamPropEnabled, 0, 0);  // disabled = read-only in UI
        gPropHost->propSetInt(props, kOfxParamPropPersistant, 0, 0);
        gPropHost->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 0);
        gPropHost->propSetInt(props, kOfxParamPropAnimates, 0, 0);
    }

    return kOfxStatOK;
}

//---------------------------------------------------------------------------//
// createInstance / destroyInstance
//---------------------------------------------------------------------------//
static OfxStatus
createInstance(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);

    OfxParamSetHandle paramSet;
    gEffectHost->getParamSet(effect, &paramSet);

    MyInstanceData *myData = new MyInstanceData;

    gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
    gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName,       &myData->outputClip, 0);

    gParamHost->paramGetHandle(paramSet, kParamWhiteOption, &myData->whiteOptionParam, 0);
    gParamHost->paramGetHandle(paramSet, kParamRange,       &myData->rangeParam,       0);
    gParamHost->paramGetHandle(paramSet, kParamLineWeight,  &myData->lineWeightParam,  0);
#ifdef SMOOTH_OFX_USE_GPU_CORE
    gParamHost->paramGetHandle(paramSet, kParamUseGpu,      &myData->useGpuParam,      0);
#endif
    gParamHost->paramGetHandle(paramSet, kParamBuildInfo,   &myData->buildInfoParam,   0);
    // buildInfo の値は describeInContext で kOfxParamPropDefault に焼き付け済み。
    // (Resolve の label-mode は paramSetValue を反映しないため。)

    gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *)myData);

    return kOfxStatOK;
}

static OfxStatus
destroyInstance(OfxImageEffectHandle effect)
{
    MyInstanceData *myData = getMyInstanceData(effect);
    if (myData) delete myData;
    return kOfxStatOK;
}

#ifdef SMOOTH_OFX_USE_RUST_CORE
//---------------------------------------------------------------------------//
// Rust core (smooth_core) ラッパ
//
// OFX のピクセル順 = {r, g, b, a} に対し、AE 由来の smooth_core は
// {alpha, red, green, blue} を期待する。バイト順を1チャネル分ローテートする
// in-place swizzle で吸収する。
//
// 適用範囲: 8bpc / 32bpc float のみ。16bpc は smooth_core 側の max_value が
// 0x8000 (AE 流) で OFX の 0xFFFF と異なるため、当面 C++ 経路を維持する。
//---------------------------------------------------------------------------//
template <typename Pixel>
static inline void
swizzleRgbaToArgbInPlace(Pixel *buf, std::size_t n)
{
    // OFX RGBA layout: byte[0]=r, byte[1]=g, byte[2]=b, byte[3]=a
    // AE  ARGB layout: byte[0]=alpha, byte[1]=red, byte[2]=green, byte[3]=blue
    // 同じ Pixel 構造体 (.r/.g/.b/.a が byte 0/1/2/3) を介して書き戻すと、
    // バイトレベルでは ARGB として解釈される。
    using S = decltype(buf->r);
    static_assert(sizeof(Pixel) == 4 * sizeof(S),
                  "smooth_core swizzle assumes 4 channels of equal scalar size");
    for (std::size_t i = 0; i < n; i++) {
        const S r = buf[i].r;
        const S g = buf[i].g;
        const S b = buf[i].b;
        const S a = buf[i].a;
        buf[i].r = a;  // byte[0] becomes alpha
        buf[i].g = r;  // byte[1] becomes red
        buf[i].b = g;  // byte[2] becomes green
        buf[i].a = b;  // byte[3] becomes blue
    }
}

template <typename Pixel>
static inline void
swizzleArgbToRgbaInPlace(Pixel *buf, std::size_t n)
{
    using S = decltype(buf->r);
    for (std::size_t i = 0; i < n; i++) {
        // 現在のバッファは ARGB として書かれている: byte[0/1/2/3] = a/r/g/b
        // これを構造体経由で読むと .r=a, .g=r, .b=g, .a=b になっている。
        const S a = buf[i].r;
        const S r = buf[i].g;
        const S g = buf[i].b;
        const S b = buf[i].a;
        buf[i].r = r;
        buf[i].g = g;
        buf[i].b = b;
        buf[i].a = a;
    }
}

// preProcess + process_row_range をまとめて呼ぶ。出力は dst (RGBA layout に
// 戻した状態) に書かれる。src は ARGB layout に置き換わる (呼び出し側はその後
// src を読まない前提で OK)。
template <typename Pixel>
static void
smoothing_via_rust(Pixel *src, Pixel *dst, int width, int height,
                   bool white_option, double range_param, double line_weight_param);

template <>
void
smoothing_via_rust<OfxRGBAColourB>(OfxRGBAColourB *src, OfxRGBAColourB *dst,
                                   int width, int height, bool white_option,
                                   double range_param, double line_weight_param)
{
    const std::size_t pixelCount = (std::size_t)width * (std::size_t)height;
    const int rowbytes = width * (int)sizeof(OfxRGBAColourB);

    swizzleRgbaToArgbInPlace<OfxRGBAColourB>(src, pixelCount);

    smooth_bbox_t bbox = {0, 0, 1, 1};
    smooth_core_preprocess_u8((void *)src, rowbytes, height,
                              white_option ? 1 : 0, &bbox);

    int x1 = bbox.left, y1 = bbox.top, x2 = bbox.right, y2 = bbox.bottom;
    if (y1 == 0)        y1 = 1;
    if (x1 == 0)        x1 = 1;
    if (x2 == width)    x2 -= 1;
    if (y2 == height)   y2 -= 1;

    std::memcpy(dst, src, sizeof(OfxRGBAColourB) * pixelCount);

    if (x2 > x1 && y2 > y1) {
        smooth_row_range_args_t args = {0};
        args.in_ptr        = (void *)src;
        args.out_ptr       = (void *)dst;
        args.width         = width;
        args.logical_width = width;
        args.height        = height;
        args.rowbytes      = rowbytes;
        args.range         = (uint32_t)(range_param * (255.0 * 4) / 100.0);
        args.line_weight   = (float)(line_weight_param / 2.0 + 0.5);
        args.j_start       = y1;
        args.j_end         = y2;
        args.i_start       = x1;
        args.i_end         = x2;
        args.parallel      = 1;  // rayon strip-parallel (smooth_core 側で 32 行未満は serial fallback)
        smooth_core_process_row_range_u8(&args);
    }

    swizzleArgbToRgbaInPlace<OfxRGBAColourB>(dst, pixelCount);
}

template <>
void
smoothing_via_rust<OfxRGBAColourF>(OfxRGBAColourF *src, OfxRGBAColourF *dst,
                                   int width, int height, bool white_option,
                                   double range_param, double line_weight_param)
{
    const std::size_t pixelCount = (std::size_t)width * (std::size_t)height;
    const int rowbytes = width * (int)sizeof(OfxRGBAColourF);

    // Rust 側 (smooth_core::Pixel32::white_key) は (1.0, 1.0, 1.0, 1.0) との
    // 厳密 == 判定。Resolve の float は色管理で 1.0 ピッタリにならない場合が
    // あるため、許容差内のピクセルを 1.0 にスナップしてから Rust に渡す。
    // (white_option=false でも処理しても害がないが、無駄を避けて gating)
    if (white_option) {
        constexpr float EPS = 0.005f;
        for (std::size_t i = 0; i < pixelCount; i++) {
            if (src[i].r > 1.0f - EPS && src[i].r < 1.0f + EPS &&
                src[i].g > 1.0f - EPS && src[i].g < 1.0f + EPS &&
                src[i].b > 1.0f - EPS && src[i].b < 1.0f + EPS)
            {
                src[i].r = 1.0f;
                src[i].g = 1.0f;
                src[i].b = 1.0f;
            }
        }
    }

    swizzleRgbaToArgbInPlace<OfxRGBAColourF>(src, pixelCount);

    smooth_bbox_t bbox = {0, 0, 1, 1};
    smooth_core_preprocess_f32((void *)src, rowbytes, height,
                               white_option ? 1 : 0, &bbox);

    int x1 = bbox.left, y1 = bbox.top, x2 = bbox.right, y2 = bbox.bottom;
    if (y1 == 0)        y1 = 1;
    if (x1 == 0)        x1 = 1;
    if (x2 == width)    x2 -= 1;
    if (y2 == height)   y2 -= 1;

    std::memcpy(dst, src, sizeof(OfxRGBAColourF) * pixelCount);

    if (x2 > x1 && y2 > y1) {
        smooth_row_range_args_f32_t args = {0};
        args.in_ptr        = (void *)src;
        args.out_ptr       = (void *)dst;
        args.width         = width;
        args.logical_width = width;
        args.height        = height;
        args.rowbytes      = rowbytes;
        args.range         = (float)(range_param * (1.0 * 4) / 100.0);
        args.line_weight   = (float)(line_weight_param / 2.0 + 0.5);
        args.j_start       = y1;
        args.j_end         = y2;
        args.i_start       = x1;
        args.i_end         = x2;
        args.parallel      = 1;
        smooth_core_process_row_range_f32(&args);
    }

    swizzleArgbToRgbaInPlace<OfxRGBAColourF>(dst, pixelCount);
}
#endif // SMOOTH_OFX_USE_RUST_CORE

//---------------------------------------------------------------------------//
// render
//---------------------------------------------------------------------------//
class NoImageEx {};

static OfxStatus
render(OfxImageEffectHandle instance,
       OfxPropertySetHandle inArgs,
       OfxPropertySetHandle /*outArgs*/)
{
    OfxTime   time;
    OfxRectI  renderWindow;
    gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

    MyInstanceData *myData = getMyInstanceData(instance);
    if (!myData) return kOfxStatErrBadHandle;

    // パラメータ取得
    int    whiteOption = 0;
    double range       = 1.0;
    double lineWeight  = 0.0;
    int    useGpu      = 1;  // default ON; ignored unless USE_GPU_CORE is built
    gParamHost->paramGetValueAtTime(myData->whiteOptionParam, time, &whiteOption);
    gParamHost->paramGetValueAtTime(myData->rangeParam,       time, &range);
    gParamHost->paramGetValueAtTime(myData->lineWeightParam,  time, &lineWeight);
#ifdef SMOOTH_OFX_USE_GPU_CORE
    gParamHost->paramGetValueAtTime(myData->useGpuParam,      time, &useGpu);
#endif

    OfxPropertySetHandle sourceImg = nullptr, outputImg = nullptr;
    OfxStatus status = kOfxStatOK;
    unsigned char *srcBuf = nullptr;
    unsigned char *dstBuf = nullptr;

    try {
        // 出力画像を取得
        if (gEffectHost->clipGetImage(myData->outputClip, time, nullptr, &outputImg) != kOfxStatOK)
            throw NoImageEx();

        int   dstRowBytes = 0;
        OfxRectI dstBounds;
        void *dstPtr = nullptr;
        char *dstDepth = nullptr, *dstComponents = nullptr;
        gPropHost->propGetInt    (outputImg, kOfxImagePropRowBytes, 0, &dstRowBytes);
        gPropHost->propGetIntN   (outputImg, kOfxImagePropBounds, 4, &dstBounds.x1);
        gPropHost->propGetPointer(outputImg, kOfxImagePropData, 0, &dstPtr);
        gPropHost->propGetString (outputImg, kOfxImageEffectPropPixelDepth, 0, &dstDepth);
        gPropHost->propGetString (outputImg, kOfxImageEffectPropComponents, 0, &dstComponents);

        // 入力画像を取得
        if (gEffectHost->clipGetImage(myData->sourceClip, time, nullptr, &sourceImg) != kOfxStatOK)
            throw NoImageEx();

        int   srcRowBytes = 0;
        OfxRectI srcBounds;
        void *srcPtr = nullptr;
        char *srcDepth = nullptr, *srcComponents = nullptr, *srcPremult = nullptr;
        gPropHost->propGetInt    (sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
        gPropHost->propGetIntN   (sourceImg, kOfxImagePropBounds, 4, &srcBounds.x1);
        gPropHost->propGetPointer(sourceImg, kOfxImagePropData, 0, &srcPtr);
        gPropHost->propGetString (sourceImg, kOfxImageEffectPropPixelDepth, 0, &srcDepth);
        gPropHost->propGetString (sourceImg, kOfxImageEffectPropComponents, 0, &srcComponents);
        gPropHost->propGetString (sourceImg, kOfxImageEffectPropPreMultiplication, 0, &srcPremult);
        const bool isPremult =
            (srcPremult && std::strcmp(srcPremult, kOfxImagePreMultiplied) == 0);

        // RGBA / 同一ビット深度を要求
        if (!srcComponents || !dstComponents ||
            std::strcmp(srcComponents, kOfxImageComponentRGBA) != 0 ||
            std::strcmp(dstComponents, kOfxImageComponentRGBA) != 0 ||
            !srcDepth || !dstDepth || std::strcmp(srcDepth, dstDepth) != 0)
        {
            status = kOfxStatErrImageFormat;
            throw NoImageEx();
        }

        const bool is16bit  = (std::strcmp(srcDepth, kOfxBitDepthShort) == 0);
        const bool isFloat  = (std::strcmp(srcDepth, kOfxBitDepthFloat) == 0);
        const int  bpp      = isFloat ? (int)sizeof(OfxRGBAColourF)
                            : is16bit ? (int)sizeof(OfxRGBAColourS)
                                      : (int)sizeof(OfxRGBAColourB);

        // 処理サイズは output のバウンズに合わせる (no-tiles なので RoD 全体)
        const int width  = dstBounds.x2 - dstBounds.x1;
        const int height = dstBounds.y2 - dstBounds.y1;
        if (width <= 0 || height <= 0) {
            status = kOfxStatOK;
            throw NoImageEx();
        }

        const size_t bufBytes = (size_t)bpp * (size_t)width * (size_t)height;
        srcBuf = (unsigned char *)std::malloc(bufBytes);
        dstBuf = (unsigned char *)std::malloc(bufBytes);
        if (!srcBuf || !dstBuf) {
            status = kOfxStatErrMemory;
            throw NoImageEx();
        }
        std::memset(srcBuf, 0, bufBytes);

        // source → srcBuf (dstBounds と同範囲の部分だけコピー、範囲外は 0 のまま)
        {
            const int copyX1 = (srcBounds.x1 > dstBounds.x1) ? srcBounds.x1 : dstBounds.x1;
            const int copyX2 = (srcBounds.x2 < dstBounds.x2) ? srcBounds.x2 : dstBounds.x2;
            const int copyY1 = (srcBounds.y1 > dstBounds.y1) ? srcBounds.y1 : dstBounds.y1;
            const int copyY2 = (srcBounds.y2 < dstBounds.y2) ? srcBounds.y2 : dstBounds.y2;
            if (copyX2 > copyX1 && copyY2 > copyY1) {
                const int copyWidth = copyX2 - copyX1;
                for (int y = copyY1; y < copyY2; y++) {
                    const unsigned char *srcRow =
                        (const unsigned char *)srcPtr
                        + (ptrdiff_t)(y - srcBounds.y1) * srcRowBytes
                        + (ptrdiff_t)(copyX1 - srcBounds.x1) * bpp;
                    unsigned char *bufRow =
                        srcBuf
                        + (ptrdiff_t)(y - dstBounds.y1) * (ptrdiff_t)width * bpp
                        + (ptrdiff_t)(copyX1 - dstBounds.x1) * bpp;
                    std::memcpy(bufRow, srcRow, (size_t)copyWidth * bpp);
                }
            }
        }

        // プリマルチプライド → ストレートに変換 (アルゴリズムは unpremultiplied 前提)
        const int pixelCount = width * height;
        if (isPremult) {
            if (isFloat)      unpremultBuffer<OfxRGBAColourF>((OfxRGBAColourF *)srcBuf, pixelCount);
            else if (is16bit) unpremultBuffer<OfxRGBAColourS>((OfxRGBAColourS *)srcBuf, pixelCount);
            else              unpremultBuffer<OfxRGBAColourB>((OfxRGBAColourB *)srcBuf, pixelCount);
        }

        // アルゴリズム実行 (srcBuf は preProcess で改変される)
        // SMOOTH_OFX_USE_RUST_CORE 定義時は 8bpc / 32bpc float を smooth_core
        // (AE 由来の Rust 実装) 経由で処理。16bpc は max_value 規約 (OFX:0xFFFF
        // vs AE:0x8000) が異なるため当面 C++ 経路を維持。
        if (isFloat) {
#ifdef SMOOTH_OFX_USE_RUST_CORE
            smoothing_via_rust<OfxRGBAColourF>((OfxRGBAColourF *)srcBuf,
                                               (OfxRGBAColourF *)dstBuf,
                                               width, height,
                                               whiteOption != 0, range, lineWeight);
#else
            smoothing<OfxRGBAColourF>((OfxRGBAColourF *)srcBuf,
                                      (OfxRGBAColourF *)dstBuf,
                                      width, height,
                                      whiteOption != 0, range, lineWeight,
                                      useGpu != 0);
#endif
        } else if (is16bit) {
            smoothing<OfxRGBAColourS>((OfxRGBAColourS *)srcBuf,
                                      (OfxRGBAColourS *)dstBuf,
                                      width, height,
                                      whiteOption != 0, range, lineWeight,
                                      useGpu != 0);
        } else {
#ifdef SMOOTH_OFX_USE_RUST_CORE
            smoothing_via_rust<OfxRGBAColourB>((OfxRGBAColourB *)srcBuf,
                                               (OfxRGBAColourB *)dstBuf,
                                               width, height,
                                               whiteOption != 0, range, lineWeight);
#else
            smoothing<OfxRGBAColourB>((OfxRGBAColourB *)srcBuf,
                                      (OfxRGBAColourB *)dstBuf,
                                      width, height,
                                      whiteOption != 0, range, lineWeight,
                                      useGpu != 0);
#endif
        }

        // 出力をホストの期待形式 (プリマルチプライド) に戻す
        if (isPremult) {
            if (isFloat)      premultBuffer<OfxRGBAColourF>((OfxRGBAColourF *)dstBuf, pixelCount);
            else if (is16bit) premultBuffer<OfxRGBAColourS>((OfxRGBAColourS *)dstBuf, pixelCount);
            else              premultBuffer<OfxRGBAColourB>((OfxRGBAColourB *)dstBuf, pixelCount);
        }

        // dstBuf → output image (renderWindow と dstBounds の交差範囲のみ)
        {
            const int wx1 = (renderWindow.x1 > dstBounds.x1) ? renderWindow.x1 : dstBounds.x1;
            const int wx2 = (renderWindow.x2 < dstBounds.x2) ? renderWindow.x2 : dstBounds.x2;
            const int wy1 = (renderWindow.y1 > dstBounds.y1) ? renderWindow.y1 : dstBounds.y1;
            const int wy2 = (renderWindow.y2 < dstBounds.y2) ? renderWindow.y2 : dstBounds.y2;
            if (wx2 > wx1 && wy2 > wy1) {
                const int copyWidth = wx2 - wx1;
                for (int y = wy1; y < wy2; y++) {
                    if (gEffectHost->abort(instance)) break;
                    const unsigned char *bufRow =
                        dstBuf
                        + (ptrdiff_t)(y - dstBounds.y1) * (ptrdiff_t)width * bpp
                        + (ptrdiff_t)(wx1 - dstBounds.x1) * bpp;
                    unsigned char *dstRow =
                        (unsigned char *)dstPtr
                        + (ptrdiff_t)(y - dstBounds.y1) * dstRowBytes
                        + (ptrdiff_t)(wx1 - dstBounds.x1) * bpp;
                    std::memcpy(dstRow, bufRow, (size_t)copyWidth * bpp);
                }
            }
        }
    }
    catch (NoImageEx &) {
        if (status == kOfxStatOK && !gEffectHost->abort(instance))
            status = kOfxStatFailed;
    }

    if (srcBuf) std::free(srcBuf);
    if (dstBuf) std::free(dstBuf);
    if (sourceImg) gEffectHost->clipReleaseImage(sourceImg);
    if (outputImg) gEffectHost->clipReleaseImage(outputImg);

    return status;
}

//---------------------------------------------------------------------------//
// pluginMain
//---------------------------------------------------------------------------//
static OfxStatus
pluginMain(const char *action,
           const void *handle,
           OfxPropertySetHandle inArgs,
           OfxPropertySetHandle outArgs)
{
    try {
        OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

        if (std::strcmp(action, kOfxActionLoad) == 0)
            return onLoad();
        if (std::strcmp(action, kOfxActionUnload) == 0)
            return onUnload();
        if (std::strcmp(action, kOfxActionDescribe) == 0)
            return describe(effect);
        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
            return describeInContext(effect, inArgs);
        if (std::strcmp(action, kOfxActionCreateInstance) == 0)
            return createInstance(effect);
        if (std::strcmp(action, kOfxActionDestroyInstance) == 0)
            return destroyInstance(effect);
        if (std::strcmp(action, kOfxImageEffectActionRender) == 0)
            return render(effect, inArgs, outArgs);
    }
    catch (std::bad_alloc &)         { return kOfxStatErrMemory; }
    catch (const std::exception &)   { return kOfxStatErrUnknown; }
    catch (int err)                  { return err; }
    catch (...)                      { return kOfxStatErrUnknown; }

    return kOfxStatReplyDefault;
}

//---------------------------------------------------------------------------//
// ホストセッター
//---------------------------------------------------------------------------//
static void
setHostFunc(OfxHost *hostStruct)
{
    gHost = hostStruct;
}

//---------------------------------------------------------------------------//
// プラグイン定義
//---------------------------------------------------------------------------//
static OfxPlugin smoothPlugin = {
    kOfxImageEffectPluginApi,
    1,
    "jp.loilo.smooth",
    1, 6,
    setHostFunc,
    pluginMain
};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }

OfxExport OfxPlugin *OfxGetPlugin(int nth)
{
    return nth == 0 ? &smoothPlugin : nullptr;
}
