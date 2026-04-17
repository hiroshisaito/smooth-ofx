//
// smooth.cpp - OFX entry point
// Port of smooth-ae (After Effects plugin) to OpenFX 1.5.1
//
// Step 3: describe / describeInContext / createInstance / destroyInstance
// Step 4: render() — 8bpc + 16bpc, tight-buffer copy + smoothing algorithm
//

#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <new>

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

//---------------------------------------------------------------------------//
// インスタンスごとに保持するデータ
//---------------------------------------------------------------------------//
struct MyInstanceData {
    OfxImageClipHandle  sourceClip;
    OfxImageClipHandle  outputClip;
    OfxParamHandle      whiteOptionParam;
    OfxParamHandle      rangeParam;
    OfxParamHandle      lineWeightParam;
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

//---------------------------------------------------------------------------//
// preProcess: 白抜き + 処理領域（extent）の検出
//   in_ptr はタイトに詰められた width*height のバッファを想定
//---------------------------------------------------------------------------//
template <typename PixelType>
static void preProcess(PixelType *in_ptr, int width, int height, OfxRectI *rect, bool is_white_trans)
{
    PixelType key, null_pixel;
    getWhitePixel(&key);
    getNullPixel(&null_pixel);

    int top = 0, left = width, right = 0, bottom = 0;
    bool top_found = false, left_found = false;

    int t = 0;
    for (int j = 0; j < height; j++)
    {
        if (!top_found) top = j;

        for (int i = 0; i < width; i++, t++)
        {
            if (is_white_trans &&
                key.r == in_ptr[t].r &&
                key.g == in_ptr[t].g &&
                key.b == in_ptr[t].b)
            {
                in_ptr[t] = null_pixel;
                continue;
            }

            if (in_ptr[t].a == 0)
                continue;

            if (!is_white_trans &&
                key.r == in_ptr[t].r &&
                key.g == in_ptr[t].g &&
                key.b == in_ptr[t].b)
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
                      double line_weight_param)
{
    // 白抜き + 領域検出
    OfxRectI extent;
    preProcess<PixelType>(in_ptr, width, height, &extent, white_option);

    // 入力を出力にコピー
    std::memcpy(out_ptr, in_ptr, sizeof(PixelType) * (size_t)width * (size_t)height);

    unsigned int range = (unsigned int)(range_param *
                         (getMaxValue<PixelType>() * 4)) / 100;
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
    gPropHost->propSetDouble(props, kOfxParamPropDisplayMax, 0, 10.0);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "range");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Color distance threshold for treating neighboring pixels as equal");
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
    gParamHost->paramGetValueAtTime(myData->whiteOptionParam, time, &whiteOption);
    gParamHost->paramGetValueAtTime(myData->rangeParam,       time, &range);
    gParamHost->paramGetValueAtTime(myData->lineWeightParam,  time, &lineWeight);

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
        char *srcDepth = nullptr, *srcComponents = nullptr;
        gPropHost->propGetInt    (sourceImg, kOfxImagePropRowBytes, 0, &srcRowBytes);
        gPropHost->propGetIntN   (sourceImg, kOfxImagePropBounds, 4, &srcBounds.x1);
        gPropHost->propGetPointer(sourceImg, kOfxImagePropData, 0, &srcPtr);
        gPropHost->propGetString (sourceImg, kOfxImageEffectPropPixelDepth, 0, &srcDepth);
        gPropHost->propGetString (sourceImg, kOfxImageEffectPropComponents, 0, &srcComponents);

        // RGBA / 同一ビット深度を要求
        if (!srcComponents || !dstComponents ||
            std::strcmp(srcComponents, kOfxImageComponentRGBA) != 0 ||
            std::strcmp(dstComponents, kOfxImageComponentRGBA) != 0 ||
            !srcDepth || !dstDepth || std::strcmp(srcDepth, dstDepth) != 0)
        {
            status = kOfxStatErrImageFormat;
            throw NoImageEx();
        }

        const bool is16bit = (std::strcmp(srcDepth, kOfxBitDepthShort) == 0);
        const int  bpp     = is16bit ? (int)sizeof(OfxRGBAColourS)
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

        // アルゴリズム実行 (srcBuf は preProcess で改変される)
        if (is16bit) {
            smoothing<OfxRGBAColourS>((OfxRGBAColourS *)srcBuf,
                                      (OfxRGBAColourS *)dstBuf,
                                      width, height,
                                      whiteOption != 0, range, lineWeight);
        } else {
            smoothing<OfxRGBAColourB>((OfxRGBAColourB *)srcBuf,
                                      (OfxRGBAColourB *)dstBuf,
                                      width, height,
                                      whiteOption != 0, range, lineWeight);
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
    1, 4,
    setHostFunc,
    pluginMain
};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }

OfxExport OfxPlugin *OfxGetPlugin(int nth)
{
    return nth == 0 ? &smoothPlugin : nullptr;
}
