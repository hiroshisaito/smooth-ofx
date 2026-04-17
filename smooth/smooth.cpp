//
// smooth.cpp - OFX entry point
// Port of smooth-ae (After Effects plugin) to OpenFX 1.5.1
//
// Step 3: describe / describeInContext / createInstance / destroyInstance
// Render action is still a stub — actual pixel processing is Step 4.
//

#include <cstring>
#include <stdexcept>
#include <new>

#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxPixels.h"

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
// describe: プラグインの基本情報をホストに伝える
//---------------------------------------------------------------------------//
static OfxStatus
describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle effectProps;
    gEffectHost->getPropertySet(effect, &effectProps);

    // ラベル / グループ
    gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Smooth");
    gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Filter");

    // コンテキスト: Filter のみサポート
    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

    // ピクセル深度: 8bit / 16bit（smooth-ae と同じ）
    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);

    // 複数ピクセル深度のクリップは扱わない
    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);

    // タイル分割は未対応（smooth は近傍参照が広いため全画像を一括取得する想定）
    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, 0);

    // マルチ解像度は未対応
    gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultiResolution, 0, 0);

    // インスタンス単位でスレッドセーフ
    gPropHost->propSetString(effectProps, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);

    return kOfxStatOK;
}

//---------------------------------------------------------------------------//
// describeInContext: クリップとパラメータの定義
//---------------------------------------------------------------------------//
static OfxStatus
describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/)
{
    OfxPropertySetHandle props;

    // 出力クリップ
    gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);

    // 入力クリップ
    gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
    gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropHost->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);

    // パラメータセット取得
    OfxParamSetHandle paramSet;
    gEffectHost->getParamSet(effect, &paramSet);

    // whiteOption: ブーリアン、"transparent" ラベル、デフォルト false
    gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kParamWhiteOption, &props);
    gPropHost->propSetInt(props, kOfxParamPropDefault, 0, 0);
    gPropHost->propSetString(props, kOfxPropLabel, 0, "transparent");
    gPropHost->propSetString(props, kOfxParamPropHint, 0, "Treat white as transparent (alpha=0 areas are skipped when off)");
    gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kParamWhiteOption);

    // range: 同じ色とみなす範囲、[0,100] 有効 / [0,10] 表示 / デフォルト 1.0
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

    // lineWeight: ラインの太さ、[0,1]、デフォルト 0.0
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
// createInstance: クリップ/パラメータハンドルをキャッシュ
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

//---------------------------------------------------------------------------//
// destroyInstance
//---------------------------------------------------------------------------//
static OfxStatus
destroyInstance(OfxImageEffectHandle effect)
{
    MyInstanceData *myData = getMyInstanceData(effect);
    if (myData) delete myData;
    return kOfxStatOK;
}

//---------------------------------------------------------------------------//
// render: Step 4 で実装する。現時点ではスタブ。
//---------------------------------------------------------------------------//
static OfxStatus
render(OfxImageEffectHandle /*instance*/,
       OfxPropertySetHandle /*inArgs*/,
       OfxPropertySetHandle /*outArgs*/)
{
    // TODO (Step 4): Source/Output イメージ取得 → BlendingInfo 構築 → upMode/downMode/8link/Lack を呼ぶ
    return kOfxStatReplyDefault;
}

//---------------------------------------------------------------------------//
// pluginMain: アクション分岐
//---------------------------------------------------------------------------//
static OfxStatus
pluginMain(const char *action,
           const void *handle,
           OfxPropertySetHandle inArgs,
           OfxPropertySetHandle outArgs)
{
    try {
        OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

        if (strcmp(action, kOfxActionLoad) == 0) {
            return onLoad();
        }
        else if (strcmp(action, kOfxActionUnload) == 0) {
            return onUnload();
        }
        else if (strcmp(action, kOfxActionDescribe) == 0) {
            return describe(effect);
        }
        else if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            return describeInContext(effect, inArgs);
        }
        else if (strcmp(action, kOfxActionCreateInstance) == 0) {
            return createInstance(effect);
        }
        else if (strcmp(action, kOfxActionDestroyInstance) == 0) {
            return destroyInstance(effect);
        }
        else if (strcmp(action, kOfxImageEffectActionRender) == 0) {
            return render(effect, inArgs, outArgs);
        }
    }
    catch (std::bad_alloc &) {
        return kOfxStatErrMemory;
    }
    catch (const std::exception &) {
        return kOfxStatErrUnknown;
    }
    catch (int err) {
        return err;
    }
    catch (...) {
        return kOfxStatErrUnknown;
    }

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
