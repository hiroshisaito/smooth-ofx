// host_smoke.cpp
// ----------------------------------------------------------------------------
// 最小限の OFX ホスト。smooth プラグインを動的に読み込み、
// setHost → onLoad → describe → describeInContext → createInstance → render
// → destroyInstance → onUnload を順に駆動し、戻り値と描画結果を確認する。
//
// 目的: DaVinci Resolve 等の実機ホストを用意する前に、プラグインが
//       「load/describe/パラメータ定義/最低限の render」までは落ちずに
//       走ることを MSYS2 MINGW64 のコマンドラインで検証する。
//       Phase 2 (Rust core) では macOS 上でも本ハーネスでピクセル統計を比較し、
//       AE 由来の Rust 移植経路と C++ 経路の同値性を確認する。
// ----------------------------------------------------------------------------

#if defined(_WIN32)
#  include <windows.h>
#  define SMOOTH_DL_HANDLE         HMODULE
#  define SMOOTH_DL_OPEN(path)     LoadLibraryA(path)
#  define SMOOTH_DL_SYM(h, name)   ((void *)GetProcAddress((h), (name)))
#  define SMOOTH_DL_CLOSE(h)       FreeLibrary(h)
#  define SMOOTH_DL_ERR_FMT        "LoadLibrary error=%lu"
#  define SMOOTH_DL_ERR_VAL        GetLastError()
#else
#  include <dlfcn.h>
#  define SMOOTH_DL_HANDLE         void *
#  define SMOOTH_DL_OPEN(path)     dlopen((path), RTLD_LAZY | RTLD_LOCAL)
#  define SMOOTH_DL_SYM(h, name)   dlsym((h), (name))
#  define SMOOTH_DL_CLOSE(h)       dlclose(h)
#  define SMOOTH_DL_ERR_FMT        "dlopen error=%s"
#  define SMOOTH_DL_ERR_VAL        (dlerror() ? dlerror() : "(none)")
#endif

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxPixels.h"
#include "ofxProperty.h"

// ---------------------------------------------------------------------------
// PropertySet: 任意の OFX プロパティを保持するシンプルな実装
// ---------------------------------------------------------------------------
struct PropertySet {
    std::map<std::string, std::vector<int>>         ints;
    std::map<std::string, std::vector<double>>      doubles;
    std::map<std::string, std::vector<std::string>> strings;
    std::map<std::string, std::vector<void *>>      pointers;

    template <class Vec, class T>
    static void ensureSize(Vec &v, int idx, T def)
    {
        if ((int)v.size() <= idx) v.resize(idx + 1, def);
    }
};

#define PROP_SET_HANDLE(ps) reinterpret_cast<OfxPropertySetHandle>(ps)
#define PROP_GET_HANDLE(h)  reinterpret_cast<PropertySet *>(h)

// ---------------------------------------------------------------------------
// Handle の簡易ラッパ (OfxImageEffectHandle / OfxImageClipHandle / OfxParamHandle)
// ---------------------------------------------------------------------------
struct ClipHandle {
    std::string                          name;
    std::unique_ptr<PropertySet>         props;       // clip 自身のプロパティ
    std::unique_ptr<PropertySet>         imageProps;  // clipGetImage が返す画像プロパティ
};

struct ParamHandle {
    std::string                          name;
    std::unique_ptr<PropertySet>         props;
};

struct EffectHandle {
    std::unique_ptr<PropertySet>         props;
    std::map<std::string, std::unique_ptr<ClipHandle>>  clips;
    std::map<std::string, std::unique_ptr<ParamHandle>> params;
};

// ---------------------------------------------------------------------------
// OfxPropertySuiteV1 実装
// ---------------------------------------------------------------------------
static OfxStatus prop_set_pointer(OfxPropertySetHandle h, const char *name, int idx, void *v)
{
    auto *p = PROP_GET_HANDLE(h); auto &a = p->pointers[name];
    PropertySet::ensureSize(a, idx, (void *)nullptr); a[idx] = v; return kOfxStatOK;
}
static OfxStatus prop_set_string(OfxPropertySetHandle h, const char *name, int idx, const char *v)
{
    auto *p = PROP_GET_HANDLE(h); auto &a = p->strings[name];
    PropertySet::ensureSize(a, idx, std::string{}); a[idx] = v ? v : ""; return kOfxStatOK;
}
static OfxStatus prop_set_double(OfxPropertySetHandle h, const char *name, int idx, double v)
{
    auto *p = PROP_GET_HANDLE(h); auto &a = p->doubles[name];
    PropertySet::ensureSize(a, idx, 0.0); a[idx] = v; return kOfxStatOK;
}
static OfxStatus prop_set_int(OfxPropertySetHandle h, const char *name, int idx, int v)
{
    auto *p = PROP_GET_HANDLE(h); auto &a = p->ints[name];
    PropertySet::ensureSize(a, idx, 0); a[idx] = v; return kOfxStatOK;
}
static OfxStatus prop_set_pointerN(OfxPropertySetHandle, const char *, int, void *const *)            { return kOfxStatOK; }
static OfxStatus prop_set_stringN (OfxPropertySetHandle, const char *, int, const char *const *)      { return kOfxStatOK; }
static OfxStatus prop_set_doubleN (OfxPropertySetHandle h, const char *name, int N, const double *v)
{
    for (int i = 0; i < N; ++i) prop_set_double(h, name, i, v[i]); return kOfxStatOK;
}
static OfxStatus prop_set_intN(OfxPropertySetHandle h, const char *name, int N, const int *v)
{
    for (int i = 0; i < N; ++i) prop_set_int(h, name, i, v[i]); return kOfxStatOK;
}

static OfxStatus prop_get_pointer(OfxPropertySetHandle h, const char *name, int idx, void **out)
{
    auto *p = PROP_GET_HANDLE(h); auto it = p->pointers.find(name);
    if (it == p->pointers.end() || (int)it->second.size() <= idx) { *out = nullptr; return kOfxStatErrUnknown; }
    *out = it->second[idx]; return kOfxStatOK;
}
static OfxStatus prop_get_string(OfxPropertySetHandle h, const char *name, int idx, char **out)
{
    auto *p = PROP_GET_HANDLE(h); auto it = p->strings.find(name);
    if (it == p->strings.end() || (int)it->second.size() <= idx) { *out = nullptr; return kOfxStatErrUnknown; }
    *out = const_cast<char *>(it->second[idx].c_str()); return kOfxStatOK;
}
static OfxStatus prop_get_double(OfxPropertySetHandle h, const char *name, int idx, double *out)
{
    auto *p = PROP_GET_HANDLE(h); auto it = p->doubles.find(name);
    if (it == p->doubles.end() || (int)it->second.size() <= idx) { *out = 0.0; return kOfxStatErrUnknown; }
    *out = it->second[idx]; return kOfxStatOK;
}
static OfxStatus prop_get_int(OfxPropertySetHandle h, const char *name, int idx, int *out)
{
    auto *p = PROP_GET_HANDLE(h); auto it = p->ints.find(name);
    if (it == p->ints.end() || (int)it->second.size() <= idx) { *out = 0; return kOfxStatErrUnknown; }
    *out = it->second[idx]; return kOfxStatOK;
}
static OfxStatus prop_get_intN(OfxPropertySetHandle h, const char *name, int N, int *out)
{
    for (int i = 0; i < N; ++i) prop_get_int(h, name, i, &out[i]); return kOfxStatOK;
}
static OfxStatus prop_get_doubleN(OfxPropertySetHandle h, const char *name, int N, double *out)
{
    for (int i = 0; i < N; ++i) prop_get_double(h, name, i, &out[i]); return kOfxStatOK;
}
static OfxStatus prop_get_stringN (OfxPropertySetHandle, const char *, int, char **)                  { return kOfxStatOK; }
static OfxStatus prop_get_pointerN(OfxPropertySetHandle, const char *, int, void **)                  { return kOfxStatOK; }
static OfxStatus prop_reset(OfxPropertySetHandle, const char *)                                       { return kOfxStatOK; }
static OfxStatus prop_get_dimension(OfxPropertySetHandle, const char *, int *d) { *d = 0; return kOfxStatOK; }

static OfxPropertySuiteV1 gPropSuite = {
    prop_set_pointer, prop_set_string, prop_set_double, prop_set_int,
    prop_set_pointerN, prop_set_stringN, prop_set_doubleN, prop_set_intN,
    prop_get_pointer, prop_get_string, prop_get_double, prop_get_int,
    prop_get_pointerN, prop_get_stringN, prop_get_doubleN, prop_get_intN,
    prop_reset, prop_get_dimension,
};

// ---------------------------------------------------------------------------
// OfxImageEffectSuiteV1 実装 (必要な関数だけ)
// ---------------------------------------------------------------------------
static OfxStatus ie_getPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle *out)
{
    auto *eff = reinterpret_cast<EffectHandle *>(h);
    *out = PROP_SET_HANDLE(eff->props.get()); return kOfxStatOK;
}
static OfxStatus ie_getParamSet(OfxImageEffectHandle h, OfxParamSetHandle *out)
{
    *out = reinterpret_cast<OfxParamSetHandle>(h); return kOfxStatOK;   // EffectHandle ごと返す
}
static OfxStatus ie_clipDefine(OfxImageEffectHandle h, const char *name, OfxPropertySetHandle *out)
{
    auto *eff = reinterpret_cast<EffectHandle *>(h);
    auto &slot = eff->clips[name];
    if (!slot) { slot.reset(new ClipHandle{name, std::make_unique<PropertySet>(), nullptr}); }
    *out = PROP_SET_HANDLE(slot->props.get()); return kOfxStatOK;
}
static OfxStatus ie_clipGetHandle(OfxImageEffectHandle h, const char *name,
                                  OfxImageClipHandle *clipOut, OfxPropertySetHandle *propsOut)
{
    auto *eff = reinterpret_cast<EffectHandle *>(h);
    auto it = eff->clips.find(name);
    if (it == eff->clips.end()) return kOfxStatErrUnknown;
    *clipOut = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (propsOut) *propsOut = PROP_SET_HANDLE(it->second->props.get());
    return kOfxStatOK;
}
static OfxStatus ie_clipGetPropertySet(OfxImageClipHandle h, OfxPropertySetHandle *out)
{
    auto *c = reinterpret_cast<ClipHandle *>(h);
    *out = PROP_SET_HANDLE(c->props.get()); return kOfxStatOK;
}
static OfxStatus ie_clipGetImage(OfxImageClipHandle h, OfxTime, const OfxRectD *,
                                 OfxPropertySetHandle *out)
{
    auto *c = reinterpret_cast<ClipHandle *>(h);
    if (!c->imageProps) return kOfxStatFailed;
    *out = PROP_SET_HANDLE(c->imageProps.get()); return kOfxStatOK;
}
static OfxStatus ie_clipReleaseImage(OfxPropertySetHandle)       { return kOfxStatOK; }
static int       ie_abort(OfxImageEffectHandle)                  { return 0; }
static OfxStatus ie_stub(...)                                    { return kOfxStatErrUnsupported; }
static OfxStatus ie_clipGetRoD(OfxImageClipHandle, OfxTime, OfxRectD *)      { return kOfxStatErrUnsupported; }

static OfxImageEffectSuiteV1 gImageEffectSuite = {
    ie_getPropertySet,
    ie_getParamSet,
    ie_clipDefine,
    ie_clipGetHandle,
    ie_clipGetPropertySet,
    ie_clipGetImage,
    ie_clipReleaseImage,
    ie_clipGetRoD,
    ie_abort,
    reinterpret_cast<OfxStatus (*)(OfxImageEffectHandle, size_t, OfxImageMemoryHandle *)>(ie_stub), // imageMemoryAlloc
    reinterpret_cast<OfxStatus (*)(OfxImageMemoryHandle)>(ie_stub),          // imageMemoryFree
    reinterpret_cast<OfxStatus (*)(OfxImageMemoryHandle, void **)>(ie_stub), // imageMemoryLock
    reinterpret_cast<OfxStatus (*)(OfxImageMemoryHandle)>(ie_stub),          // imageMemoryUnlock
};

// ---------------------------------------------------------------------------
// OfxParameterSuiteV1 実装
// ---------------------------------------------------------------------------
static OfxStatus param_define(OfxParamSetHandle h, const char *type, const char *name,
                              OfxPropertySetHandle *out)
{
    auto *eff = reinterpret_cast<EffectHandle *>(h);
    auto &slot = eff->params[name];
    if (!slot) { slot.reset(new ParamHandle{name, std::make_unique<PropertySet>()}); }
    // type を参照プロパティとして記録
    prop_set_string(PROP_SET_HANDLE(slot->props.get()), kOfxParamPropType, 0, type);
    *out = PROP_SET_HANDLE(slot->props.get()); return kOfxStatOK;
}
static OfxStatus param_get_handle(OfxParamSetHandle h, const char *name,
                                  OfxParamHandle *out, OfxPropertySetHandle *propsOut)
{
    auto *eff = reinterpret_cast<EffectHandle *>(h);
    auto it = eff->params.find(name);
    if (it == eff->params.end()) return kOfxStatErrUnknown;
    *out = reinterpret_cast<OfxParamHandle>(it->second.get());
    if (propsOut) *propsOut = PROP_SET_HANDLE(it->second->props.get());
    return kOfxStatOK;
}
static OfxStatus param_get_props(OfxParamHandle h, OfxPropertySetHandle *out)
{
    auto *p = reinterpret_cast<ParamHandle *>(h);
    *out = PROP_SET_HANDLE(p->props.get()); return kOfxStatOK;
}
// paramGetValue / paramGetValueAtTime: デフォルト値 (kOfxParamPropDefault) を返す。可変長引数。
static OfxStatus param_get_value(OfxParamHandle h, ...)
{
    auto *p = reinterpret_cast<ParamHandle *>(h);
    std::string type;
    { char *t = nullptr; prop_get_string(PROP_SET_HANDLE(p->props.get()), kOfxParamPropType, 0, &t); if (t) type = t; }

    va_list ap; va_start(ap, h);
    if (type == kOfxParamTypeBoolean || type == kOfxParamTypeInteger) {
        int *dst = va_arg(ap, int *);
        int def = 0; prop_get_int(PROP_SET_HANDLE(p->props.get()), kOfxParamPropDefault, 0, &def);
        if (dst) *dst = def;
    } else if (type == kOfxParamTypeDouble) {
        double *dst = va_arg(ap, double *);
        double def = 0.0; prop_get_double(PROP_SET_HANDLE(p->props.get()), kOfxParamPropDefault, 0, &def);
        if (dst) *dst = def;
    } else if (type == kOfxParamTypeRGBA || type == kOfxParamTypeRGB ||
               type == kOfxParamTypeDouble2D || type == kOfxParamTypeDouble3D) {
        // 未使用だがクラッシュ防止
        for (int i = 0; i < 4; ++i) (void)va_arg(ap, double *);
    }
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus param_get_value_at_time(OfxParamHandle h, OfxTime t, ...)
{
    auto *p = reinterpret_cast<ParamHandle *>(h);
    std::string type;
    { char *ts = nullptr; prop_get_string(PROP_SET_HANDLE(p->props.get()), kOfxParamPropType, 0, &ts); if (ts) type = ts; }
    (void)t;

    va_list ap; va_start(ap, t);
    if (type == kOfxParamTypeBoolean || type == kOfxParamTypeInteger) {
        int *dst = va_arg(ap, int *);
        int def = 0; prop_get_int(PROP_SET_HANDLE(p->props.get()), kOfxParamPropDefault, 0, &def);
        if (dst) *dst = def;
    } else if (type == kOfxParamTypeDouble) {
        double *dst = va_arg(ap, double *);
        double def = 0.0; prop_get_double(PROP_SET_HANDLE(p->props.get()), kOfxParamPropDefault, 0, &def);
        if (dst) *dst = def;
    }
    va_end(ap);
    return kOfxStatOK;
}

// paramSetValue: 文字列だけ最低限実装 (buildInfo ラベル更新確認用)。
// 数値/ブール系は smooth プラグインが render 時に書き込まないため未対応で OK。
static OfxStatus param_set_value(OfxParamHandle h, ...)
{
    auto *p = reinterpret_cast<ParamHandle *>(h);
    std::string type;
    { char *ts = nullptr; prop_get_string(PROP_SET_HANDLE(p->props.get()), kOfxParamPropType, 0, &ts); if (ts) type = ts; }

    va_list ap; va_start(ap, h);
    if (type == kOfxParamTypeString) {
        const char *v = va_arg(ap, const char *);
        if (v) {
            // 値を kOfxParamPropDefault 経由で再現 (param_get_value がそれを返すため)
            prop_set_string(PROP_SET_HANDLE(p->props.get()), kOfxParamPropDefault, 0, v);
        }
    }
    va_end(ap);
    return kOfxStatOK;
}

static OfxStatus param_stub(...) { return kOfxStatErrUnsupported; }

static OfxParameterSuiteV1 gParamSuite = {
    param_define,
    param_get_handle,
    reinterpret_cast<OfxStatus (*)(OfxParamSetHandle, OfxPropertySetHandle *)>(param_stub),    // paramSetGetPropertySet
    param_get_props,
    param_get_value,
    param_get_value_at_time,
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxTime, ...)>(param_stub),                 // paramGetDerivative
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxTime, OfxTime, ...)>(param_stub),        // paramGetIntegral
    param_set_value,                                                                            // paramSetValue
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxTime, ...)>(param_stub),                 // paramSetValueAtTime
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, unsigned int *)>(param_stub),               // paramGetNumKeys
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, unsigned int, OfxTime *)>(param_stub),      // paramGetKeyTime
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxTime, int, int *)>(param_stub),          // paramGetKeyIndex
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxTime)>(param_stub),                      // paramDeleteKey
    reinterpret_cast<OfxStatus (*)(OfxParamHandle)>(param_stub),                               // paramDeleteAllKeys
    reinterpret_cast<OfxStatus (*)(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD *)>(param_stub), // paramCopy
    reinterpret_cast<OfxStatus (*)(OfxParamSetHandle, const char *)>(param_stub),              // paramEditBegin
    reinterpret_cast<OfxStatus (*)(OfxParamSetHandle)>(param_stub),                            // paramEditEnd
};

// ---------------------------------------------------------------------------
// ホスト情報 + fetchSuite
// ---------------------------------------------------------------------------
static PropertySet gHostProps;

static const void *host_fetch_suite(OfxPropertySetHandle, const char *name, int)
{
    if (std::strcmp(name, kOfxPropertySuite)   == 0) return &gPropSuite;
    if (std::strcmp(name, kOfxImageEffectSuite) == 0) return &gImageEffectSuite;
    if (std::strcmp(name, kOfxParameterSuite)   == 0) return &gParamSuite;
    return nullptr;
}

static OfxHost gHost = {
    PROP_SET_HANDLE(&gHostProps),
    host_fetch_suite,
};

// ---------------------------------------------------------------------------
// 画像バッファ作成ヘルパ (8bit RGBA、ストライプのテスト画像)
// ---------------------------------------------------------------------------
template <typename P>
static void make_test_image(P *buf, int w, int h, unsigned int maxv)
{
    // 斜め階段パターン: smooth プラグインが境界の階段をアンチエイリアス化する。
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int step = x + y;
            P p;
            if ((step / 4) % 2 == 0) { p.r = p.g = p.b = 0;                           p.a = (decltype(p.a))maxv; }
            else                      { p.r = p.g = p.b = (decltype(p.r))maxv;        p.a = (decltype(p.a))maxv; }
            buf[y * w + x] = p;
        }
    }
}

// PPM (P6) 出力。16bpc は maxval=65535 の P6 として書き出し (ネットワークバイト順)
template <typename P>
static bool write_ppm(const char *path, const P *buf, int w, int h, unsigned int maxv)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n%u\n", w, h, maxv);
    for (int i = 0; i < w * h; ++i) {
        if (maxv <= 255) {
            unsigned char rgb[3] = { (unsigned char)buf[i].r, (unsigned char)buf[i].g, (unsigned char)buf[i].b };
            std::fwrite(rgb, 1, 3, f);
        } else {
            // P6 16bit はビッグエンディアン
            unsigned short rgb[3] = {
                (unsigned short)(((buf[i].r & 0xFF) << 8) | ((buf[i].r >> 8) & 0xFF)),
                (unsigned short)(((buf[i].g & 0xFF) << 8) | ((buf[i].g >> 8) & 0xFF)),
                (unsigned short)(((buf[i].b & 0xFF) << 8) | ((buf[i].b >> 8) & 0xFF)),
            };
            std::fwrite(rgb, 2, 3, f);
        }
    }
    std::fclose(f);
    return true;
}

// float 版: 0..1 を 8bpc に量子化して P6 で書き出す (PPM は float 非対応)
template <>
bool write_ppm<OfxRGBAColourF>(const char *path, const OfxRGBAColourF *buf, int w, int h, unsigned int)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        auto q = [](float v) {
            if (v <= 0.0f) return (unsigned char)0;
            if (v >= 1.0f) return (unsigned char)255;
            return (unsigned char)(v * 255.0f + 0.5f);
        };
        unsigned char rgb[3] = { q(buf[i].r), q(buf[i].g), q(buf[i].b) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// 画像プロパティ (clipGetImage の戻り) を仕込む
// ---------------------------------------------------------------------------
static void setup_image_props(PropertySet &ps, void *data, int w, int h,
                              const char *pixelDepth, const char *components,
                              int rowBytes)
{
    prop_set_pointer(PROP_SET_HANDLE(&ps), kOfxImagePropData, 0, data);
    int bounds[4] = { 0, 0, w, h };
    prop_set_intN(PROP_SET_HANDLE(&ps), kOfxImagePropBounds, 4, bounds);
    prop_set_intN(PROP_SET_HANDLE(&ps), kOfxImagePropRegionOfDefinition, 4, bounds);
    prop_set_int(PROP_SET_HANDLE(&ps), kOfxImagePropRowBytes, 0, rowBytes);
    prop_set_string(PROP_SET_HANDLE(&ps), kOfxImageEffectPropPixelDepth, 0, pixelDepth);
    prop_set_string(PROP_SET_HANDLE(&ps), kOfxImageEffectPropComponents, 0, components);
}

// ---------------------------------------------------------------------------
// パラメータ一覧のダンプ (describeInContext 後)
// ---------------------------------------------------------------------------
static void dump_params(const EffectHandle &eff)
{
    for (auto &kv : eff.params) {
        auto *props = kv.second->props.get();
        char *type = nullptr, *label = nullptr;
        prop_get_string(PROP_SET_HANDLE(props), kOfxParamPropType, 0, &type);
        prop_get_string(PROP_SET_HANDLE(props), kOfxPropLabel,      0, &label);
        std::printf("    param: name=%-14s type=%-8s label=%s\n",
                    kv.first.c_str(),
                    type ? type : "?",
                    label ? label : "?");
    }
}
static void dump_clips(const EffectHandle &eff)
{
    for (auto &kv : eff.clips) {
        std::printf("    clip:  name=%s\n", kv.first.c_str());
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const char *dllPath = (argc > 1) ? argv[1] : "build-mingw/smooth.ofx";
    std::printf("[host-smoke] loading %s\n", dllPath);

    SMOOTH_DL_HANDLE dll = SMOOTH_DL_OPEN(dllPath);
    if (!dll) { std::printf("FAIL: " SMOOTH_DL_ERR_FMT "\n", SMOOTH_DL_ERR_VAL); return 1; }

    typedef int           (*GetNumFn)(void);
    typedef OfxPlugin *   (*GetPluginFn)(int);
    GetNumFn    getNum    = (GetNumFn)    SMOOTH_DL_SYM(dll, "OfxGetNumberOfPlugins");
    GetPluginFn getPlugin = (GetPluginFn) SMOOTH_DL_SYM(dll, "OfxGetPlugin");
    if (!getNum || !getPlugin) { std::printf("FAIL: missing exports\n"); return 2; }

    int n = getNum();
    std::printf("[host-smoke] OfxGetNumberOfPlugins = %d\n", n);
    if (n <= 0) { std::printf("FAIL: no plugins\n"); return 3; }

    OfxPlugin *plugin = getPlugin(0);
    if (!plugin) { std::printf("FAIL: OfxGetPlugin(0)=null\n"); return 4; }
    std::printf("[host-smoke] plugin: api=%s v=%u id=%s ver=%u.%u\n",
                plugin->pluginApi, plugin->apiVersion, plugin->pluginIdentifier,
                plugin->pluginVersionMajor, plugin->pluginVersionMinor);

    plugin->setHost(&gHost);

    OfxStatus st;

    // onLoad
    st = plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
    std::printf("[host-smoke] kOfxActionLoad -> %d\n", st);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) { std::printf("FAIL load\n"); return 5; }

    // describe: effect ハンドルを用意
    EffectHandle eff; eff.props = std::make_unique<PropertySet>();
    PropertySet inArgs;
    st = plugin->mainEntry(kOfxActionDescribe, &eff, nullptr, nullptr);
    std::printf("[host-smoke] kOfxActionDescribe -> %d\n", st);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) { std::printf("FAIL describe\n"); return 6; }

    // describeInContext
    prop_set_string(PROP_SET_HANDLE(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    st = plugin->mainEntry(kOfxImageEffectActionDescribeInContext, &eff,
                           PROP_SET_HANDLE(&inArgs), nullptr);
    std::printf("[host-smoke] kOfxImageEffectActionDescribeInContext -> %d\n", st);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) { std::printf("FAIL describeInContext\n"); return 7; }

    std::printf("[host-smoke] defined clips/params:\n");
    dump_clips(eff);
    dump_params(eff);

    // createInstance
    prop_set_string(PROP_SET_HANDLE(eff.props.get()), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    st = plugin->mainEntry(kOfxActionCreateInstance, &eff, nullptr, nullptr);
    std::printf("[host-smoke] kOfxActionCreateInstance -> %d\n", st);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) { std::printf("FAIL createInstance\n"); return 8; }

    // buildInfo (read-only label) を読み出して表示。createInstance 内で paramSetValue
    // されているので、それが反映されていれば runtime build identity が確認できる。
    {
        auto it = eff.params.find("buildInfo");
        if (it != eff.params.end()) {
            char *bi = nullptr;
            prop_get_string(PROP_SET_HANDLE(it->second->props.get()),
                            kOfxParamPropDefault, 0, &bi);
            std::printf("[host-smoke] buildInfo = \"%s\"\n", bi ? bi : "(null)");
        }
    }

    // 合成画像を用意して render を呼ぶ (8bpc と 16bpc を両方駆動)
    const int W = 64, H = 32;
    auto &srcClip = eff.clips[kOfxImageEffectSimpleSourceClipName];
    auto &dstClip = eff.clips[kOfxImageEffectOutputClipName];
    if (!srcClip || !dstClip) { std::printf("FAIL: clips not defined\n"); return 9; }

    // ---- 8bpc パス ----
    {
        std::vector<OfxRGBAColourB> srcImg(W * H), dstImg(W * H);
        make_test_image<OfxRGBAColourB>(srcImg.data(), W, H, 0xFF);
        std::memset(dstImg.data(), 0, sizeof(OfxRGBAColourB) * W * H);

        srcClip->imageProps = std::make_unique<PropertySet>();
        dstClip->imageProps = std::make_unique<PropertySet>();
        setup_image_props(*srcClip->imageProps, srcImg.data(), W, H,
                          kOfxBitDepthByte, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourB));
        setup_image_props(*dstClip->imageProps, dstImg.data(), W, H,
                          kOfxBitDepthByte, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourB));

        PropertySet renderArgs;
        prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
        int rw[4] = { 0, 0, W, H };
        prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

        st = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                               PROP_SET_HANDLE(&renderArgs), nullptr);
        std::printf("[host-smoke] [ 8bpc] kOfxImageEffectActionRender -> %d\n", st);

        int pure0 = 0, pureMax = 0, intermed = 0;
        for (auto &p : dstImg) {
            if (p.r == 0 && p.g == 0 && p.b == 0) ++pure0;
            else if (p.r == 0xFF && p.g == 0xFF && p.b == 0xFF) ++pureMax;
            else if (p.r == p.g && p.g == p.b) ++intermed;
        }
        std::printf("[host-smoke] [ 8bpc] pure0=%d pureMax=%d intermediate=%d / %d\n",
                    pure0, pureMax, intermed, W * H);

        write_ppm<OfxRGBAColourB>("build-mingw/smoke_src_8bpc.ppm", srcImg.data(), W, H, 0xFF);
        write_ppm<OfxRGBAColourB>("build-mingw/smoke_dst_8bpc.ppm", dstImg.data(), W, H, 0xFF);
    }

    // ---- 16bpc パス ----
    {
        std::vector<OfxRGBAColourS> srcImg(W * H), dstImg(W * H);
        make_test_image<OfxRGBAColourS>(srcImg.data(), W, H, 0xFFFF);
        std::memset(dstImg.data(), 0, sizeof(OfxRGBAColourS) * W * H);

        srcClip->imageProps = std::make_unique<PropertySet>();
        dstClip->imageProps = std::make_unique<PropertySet>();
        setup_image_props(*srcClip->imageProps, srcImg.data(), W, H,
                          kOfxBitDepthShort, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourS));
        setup_image_props(*dstClip->imageProps, dstImg.data(), W, H,
                          kOfxBitDepthShort, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourS));

        PropertySet renderArgs;
        prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
        int rw[4] = { 0, 0, W, H };
        prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

        st = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                               PROP_SET_HANDLE(&renderArgs), nullptr);
        std::printf("[host-smoke] [16bpc] kOfxImageEffectActionRender -> %d\n", st);

        int pure0 = 0, pureMax = 0, intermed = 0;
        for (auto &p : dstImg) {
            if (p.r == 0 && p.g == 0 && p.b == 0) ++pure0;
            else if (p.r == 0xFFFF && p.g == 0xFFFF && p.b == 0xFFFF) ++pureMax;
            else if (p.r == p.g && p.g == p.b) ++intermed;
        }
        std::printf("[host-smoke] [16bpc] pure0=%d pureMax=%d intermediate=%d / %d\n",
                    pure0, pureMax, intermed, W * H);

        write_ppm<OfxRGBAColourS>("build-mingw/smoke_src_16bpc.ppm", srcImg.data(), W, H, 0xFFFF);
        write_ppm<OfxRGBAColourS>("build-mingw/smoke_dst_16bpc.ppm", dstImg.data(), W, H, 0xFFFF);
    }

    // ---- float パス ----
    {
        std::vector<OfxRGBAColourF> srcImg(W * H), dstImg(W * H);
        make_test_image<OfxRGBAColourF>(srcImg.data(), W, H, 1);
        std::memset(dstImg.data(), 0, sizeof(OfxRGBAColourF) * W * H);

        srcClip->imageProps = std::make_unique<PropertySet>();
        dstClip->imageProps = std::make_unique<PropertySet>();
        setup_image_props(*srcClip->imageProps, srcImg.data(), W, H,
                          kOfxBitDepthFloat, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourF));
        setup_image_props(*dstClip->imageProps, dstImg.data(), W, H,
                          kOfxBitDepthFloat, kOfxImageComponentRGBA, W * (int)sizeof(OfxRGBAColourF));

        PropertySet renderArgs;
        prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
        int rw[4] = { 0, 0, W, H };
        prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

        st = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                               PROP_SET_HANDLE(&renderArgs), nullptr);
        std::printf("[host-smoke] [float] kOfxImageEffectActionRender -> %d\n", st);

        int pure0 = 0, pureMax = 0, intermed = 0;
        for (auto &p : dstImg) {
            if (p.r == 0.0f && p.g == 0.0f && p.b == 0.0f) ++pure0;
            else if (p.r == 1.0f && p.g == 1.0f && p.b == 1.0f) ++pureMax;
            else if (p.r == p.g && p.g == p.b) ++intermed;
        }
        std::printf("[host-smoke] [float] pure0=%d pureMax=%d intermediate=%d / %d\n",
                    pure0, pureMax, intermed, W * H);

        write_ppm<OfxRGBAColourF>("build-mingw/smoke_src_float.ppm", srcImg.data(), W, H, 1);
        write_ppm<OfxRGBAColourF>("build-mingw/smoke_dst_float.ppm", dstImg.data(), W, H, 1);
    }

    // ----------------------------------------------------------------------
    // 診断モード: SMOOTH_DIAG=transparent
    //   全白画像 + whiteOption=true で render を 8bpc / 16bpc / float の 3 経路に対して走らせ、
    //   dst の null pixel 数 (RGB すべて 0) を集計して transparent オプションが
    //   各経路で機能しているか確認する。Resolve で 16/float の transparent が NG と
    //   報告されたが、それがホスト側 (実際に届く white の値が異なる) なのか
    //   プラグイン側 (Rust 経路 / 16bpc C++ 経路のバグ) なのかを切り分けるため。
    // ----------------------------------------------------------------------
    if (const char *diag = std::getenv("SMOOTH_DIAG")) {
        if (std::strcmp(diag, "gpu_passthrough") == 0) {
            // Phase E: dlsym the smooth_gpu_passthrough_u32 entry from the
            // OFX bundle and round-trip a u32 buffer. Verifies the full
            // chain host_smoke → smooth.ofx → libsmooth_gpu.a → GPU device.
            // Only meaningful when the bundle was built with USE_GPU_CORE=ON;
            // dlsym returns nullptr otherwise and we skip with a clear note.
            using PassthroughFn = uint32_t (*)(const uint32_t *, uint32_t *, std::size_t);
            using InitFn        = uint32_t (*)(void);
            using BuildIdFn     = const char *(*)(void);

            auto pt    = (PassthroughFn) SMOOTH_DL_SYM(dll, "smooth_gpu_passthrough_u32");
            auto init  = (InitFn)        SMOOTH_DL_SYM(dll, "smooth_gpu_init");
            auto bid   = (BuildIdFn)     SMOOTH_DL_SYM(dll, "smooth_gpu_build_id");

            if (!pt || !init) {
                std::printf("[host-diag] [gpu_passthrough] smooth_gpu_* not exported (USE_GPU_CORE=OFF?), skipped\n");
            } else {
                uint32_t initStatus = init();
                std::printf("[host-diag] [gpu_passthrough] smooth_gpu_init -> %u (build_id=%s)\n",
                            initStatus, bid ? bid() : "?");
                if (initStatus != 0) {
                    std::printf("[host-diag] [gpu_passthrough] init failed, skipping passthrough test\n");
                } else {
                    const std::size_t N = 1024;
                    std::vector<uint32_t> src(N), dst(N, 0);
                    for (std::size_t i = 0; i < N; ++i) src[i] = (uint32_t)(i * 0x9E3779B1u);
                    uint32_t st = pt(src.data(), dst.data(), N);
                    bool eq = (src == dst);
                    std::printf("[host-diag] [gpu_passthrough] N=%zu -> status=%u byte_identical=%s\n",
                                N, st, eq ? "yes" : "NO");
                }
            }
        } else if (std::strcmp(diag, "range") == 0) {
            // range の効力確認: 既存の 64x32 対角ストライプ画像 (smooth が確実に反応する) に
            // 対し range を 0/1/5/10/50/100 で render し、dst の intermediate ピクセル
            // (R==G==B かつ非 0 / 非 max) 数の変化を観察する。range が効いていれば
            // 値変化に応じて分布が変わるはず。
            const int W = 64, H = 32;

            auto run = [&](const char *label, auto pixel_tag, const char *bitDepth, unsigned int maxv, double rangeVal) {
                using Pixel = decltype(pixel_tag);
                std::vector<Pixel> srcImg(W * H), dstImg(W * H);
                make_test_image<Pixel>(srcImg.data(), W, H, maxv);
                std::memset(dstImg.data(), 0, sizeof(Pixel) * W * H);

                auto rIt = eff.params.find("range");
                if (rIt != eff.params.end()) {
                    prop_set_double(PROP_SET_HANDLE(rIt->second->props.get()),
                                    kOfxParamPropDefault, 0, rangeVal);
                }

                srcClip->imageProps = std::make_unique<PropertySet>();
                dstClip->imageProps = std::make_unique<PropertySet>();
                setup_image_props(*srcClip->imageProps, srcImg.data(), W, H,
                                  bitDepth, kOfxImageComponentRGBA, W * (int)sizeof(Pixel));
                setup_image_props(*dstClip->imageProps, dstImg.data(), W, H,
                                  bitDepth, kOfxImageComponentRGBA, W * (int)sizeof(Pixel));

                PropertySet renderArgs;
                prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
                int rw[4] = { 0, 0, W, H };
                prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

                OfxStatus rs = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                                                 PROP_SET_HANDLE(&renderArgs), nullptr);
                int pure0 = 0, pureMax = 0, intermed = 0;
                for (auto &p : dstImg) {
                    if (p.r == (decltype(p.r))0 && p.g == (decltype(p.g))0 && p.b == (decltype(p.b))0) ++pure0;
                    else if (p.r == (decltype(p.r))maxv && p.g == (decltype(p.g))maxv && p.b == (decltype(p.b))maxv) ++pureMax;
                    else if (p.r == p.g && p.g == p.b) ++intermed;
                }
                std::printf("[host-diag] [%s range=%6.1f] render=%d pure0=%d pureMax=%d intermediate=%d / %d\n",
                            label, rangeVal, rs, pure0, pureMax, intermed, W * H);
            };

            std::printf("[host-diag] running range diagnostic (diagonal stripes %dx%d)\n", W, H);
            for (double rv : {0.0, 1.0, 5.0, 10.0, 50.0, 100.0}) {
                run(" 8bpc", OfxRGBAColourB{}, kOfxBitDepthByte,  0xFFu,    rv);
                run("16bpc", OfxRGBAColourS{}, kOfxBitDepthShort, 0xFFFFu,  rv);
                run("float", OfxRGBAColourF{}, kOfxBitDepthFloat, 1u,       rv);
            }
        } else if (std::strcmp(diag, "transparent") == 0) {
            // whiteOption の default を 1 (true) に上書き
            auto woIt = eff.params.find("whiteOption");
            if (woIt != eff.params.end()) {
                prop_set_int(PROP_SET_HANDLE(woIt->second->props.get()),
                             kOfxParamPropDefault, 0, 1);
            }

            const int W = 16, H = 8;

            auto run = [&](const char *label, auto pixel_tag, const char *bitDepth, double maxv) {
                using Pixel = decltype(pixel_tag);
                std::vector<Pixel> srcImg(W * H), dstImg(W * H);
                // 全ピクセルを白に
                for (auto &p : srcImg) {
                    p.r = (decltype(p.r))maxv;
                    p.g = (decltype(p.g))maxv;
                    p.b = (decltype(p.b))maxv;
                    p.a = (decltype(p.a))maxv;
                }
                std::memset(dstImg.data(), 0, sizeof(Pixel) * W * H);

                srcClip->imageProps = std::make_unique<PropertySet>();
                dstClip->imageProps = std::make_unique<PropertySet>();
                setup_image_props(*srcClip->imageProps, srcImg.data(), W, H,
                                  bitDepth, kOfxImageComponentRGBA, W * (int)sizeof(Pixel));
                setup_image_props(*dstClip->imageProps, dstImg.data(), W, H,
                                  bitDepth, kOfxImageComponentRGBA, W * (int)sizeof(Pixel));

                PropertySet renderArgs;
                prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
                int rw[4] = { 0, 0, W, H };
                prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

                OfxStatus rs = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                                                 PROP_SET_HANDLE(&renderArgs), nullptr);
                int nulled = 0, kept = 0;
                for (auto &p : dstImg) {
                    if (p.r == (decltype(p.r))0 && p.g == (decltype(p.g))0 && p.b == (decltype(p.b))0) ++nulled;
                    else ++kept;
                }
                std::printf("[host-diag] [%s] white_option=1 all-white -> render=%d nulled=%d kept=%d / %d\n",
                            label, rs, nulled, kept, W * H);
            };

            std::printf("[host-diag] running transparent diagnostic (all-white %dx%d, whiteOption=1)\n", W, H);
            run(" 8bpc / pure0xFF",       OfxRGBAColourB{}, kOfxBitDepthByte,  255.0);
            run("16bpc / pure0xFFFF",     OfxRGBAColourS{}, kOfxBitDepthShort, 65535.0);
            run("16bpc / AE-style0x8000", OfxRGBAColourS{}, kOfxBitDepthShort, 32768.0);
            run("16bpc / drift0xFEFE",    OfxRGBAColourS{}, kOfxBitDepthShort, 65278.0);
            run("float / pure1.0",        OfxRGBAColourF{}, kOfxBitDepthFloat, 1.0);
            run("float / drift0.998",     OfxRGBAColourF{}, kOfxBitDepthFloat, 0.998);
            run("float / drift1.003",     OfxRGBAColourF{}, kOfxBitDepthFloat, 1.003);
        }
    }

    // ----------------------------------------------------------------------
    // Bench モード (env var SMOOTH_BENCH_SIZE=WxH, SMOOTH_BENCH_ITERS=N で起動)
    // 8bpc と float の render を N 回 wall-clock 計測。median/min/max を出す。
    // 既存のスモーク 3 パス完了後に走らせるので、副作用は計測のみ。
    // ----------------------------------------------------------------------
    if (const char *sizeStr = std::getenv("SMOOTH_BENCH_SIZE")) {
        int benchW = 1920, benchH = 1080;
        if (const char *x = std::strchr(sizeStr, 'x')) {
            benchW = std::atoi(sizeStr);
            benchH = std::atoi(x + 1);
        } else if (const char *X = std::strchr(sizeStr, 'X')) {
            benchW = std::atoi(sizeStr);
            benchH = std::atoi(X + 1);
        }
        if (benchW <= 0 || benchH <= 0) { benchW = 1920; benchH = 1080; }

        int iters = 30;
        if (const char *itersStr = std::getenv("SMOOTH_BENCH_ITERS")) {
            int v = std::atoi(itersStr);
            if (v > 0) iters = v;
        }

        auto run_bench = [&](const char *label, auto pixel_tag, const char *bitDepth, double maxv) {
            using Pixel = decltype(pixel_tag);
            const std::size_t pixelCount = (std::size_t)benchW * (std::size_t)benchH;
            std::vector<Pixel> srcImg(pixelCount);
            std::vector<Pixel> dstImg(pixelCount);
            make_test_image<Pixel>(srcImg.data(), benchW, benchH, (unsigned int)maxv);

            srcClip->imageProps = std::make_unique<PropertySet>();
            dstClip->imageProps = std::make_unique<PropertySet>();
            const int rowbytes = benchW * (int)sizeof(Pixel);
            setup_image_props(*srcClip->imageProps, srcImg.data(), benchW, benchH,
                              bitDepth, kOfxImageComponentRGBA, rowbytes);
            setup_image_props(*dstClip->imageProps, dstImg.data(), benchW, benchH,
                              bitDepth, kOfxImageComponentRGBA, rowbytes);

            PropertySet renderArgs;
            prop_set_double(PROP_SET_HANDLE(&renderArgs), kOfxPropTime, 0, 0.0);
            int rw[4] = { 0, 0, benchW, benchH };
            prop_set_intN(PROP_SET_HANDLE(&renderArgs), kOfxImageEffectPropRenderWindow, 4, rw);

            std::vector<double> ms; ms.reserve(iters);
            for (int i = 0; i < iters; ++i) {
                // src は preProcess 等で改変されるので毎回作り直す
                make_test_image<Pixel>(srcImg.data(), benchW, benchH, (unsigned int)maxv);

                auto t0 = std::chrono::steady_clock::now();
                OfxStatus rs = plugin->mainEntry(kOfxImageEffectActionRender, &eff,
                                                 PROP_SET_HANDLE(&renderArgs), nullptr);
                auto t1 = std::chrono::steady_clock::now();
                if (rs != kOfxStatOK && rs != kOfxStatReplyDefault) {
                    std::printf("[host-bench] [%s] render iter %d failed -> %d\n", label, i, rs);
                    return;
                }
                double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                ms.push_back(elapsed_ms);
            }
            std::sort(ms.begin(), ms.end());
            double sum = 0; for (double v : ms) sum += v;
            double mean   = sum / ms.size();
            double median = ms[ms.size() / 2];
            double minv   = ms.front();
            double maxv2  = ms.back();
            double mp_per_s = (double)pixelCount / 1e6 / (median / 1000.0);
            std::printf("[host-bench] [%s %dx%d N=%d] min=%.2fms median=%.2fms mean=%.2fms max=%.2fms => %.1f Mpx/s\n",
                        label, benchW, benchH, iters, minv, median, mean, maxv2, mp_per_s);
        };

        std::printf("[host-bench] running %dx%d x %d iters\n", benchW, benchH, iters);
        run_bench(" 8bpc", OfxRGBAColourB{}, kOfxBitDepthByte,  255.0);
        run_bench(" 16bpc", OfxRGBAColourS{}, kOfxBitDepthShort, 65535.0);
        run_bench("float", OfxRGBAColourF{}, kOfxBitDepthFloat, 1.0);
    }

    // destroyInstance
    st = plugin->mainEntry(kOfxActionDestroyInstance, &eff, nullptr, nullptr);
    std::printf("[host-smoke] kOfxActionDestroyInstance -> %d\n", st);

    // onUnload
    st = plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("[host-smoke] kOfxActionUnload -> %d\n", st);

    SMOOTH_DL_CLOSE(dll);
    std::printf("[host-smoke] DONE\n");
    return 0;
}
