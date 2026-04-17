//
// smooth.cpp - OFX entry point
// Port of smooth-ae (After Effects plugin) to OpenFX 1.5.1
//
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"

// TODO: implement plugin

static OfxHost *gHost = nullptr;

static void setHostFunc(OfxHost *hostStruct)
{
    gHost = hostStruct;
}

static OfxStatus pluginMain(const char * /*action*/,
                             const void * /*handle*/,
                             OfxPropertySetHandle /*inArgs*/,
                             OfxPropertySetHandle /*outArgs*/)
{
    return kOfxStatReplyDefault;
}

static OfxPlugin smoothPlugin = {
    kOfxImageEffectPluginApi,
    1,
    "jp.loilo.smooth",
    1, 4,
    setHostFunc,
    pluginMain
};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &smoothPlugin : nullptr; }
