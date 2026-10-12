#include <Python.h>
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <atomic>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "ofxhPluginCache.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhParam.h"
#include "ofxParametricParam.h"
#include "ofxNatron.h"
#include "ofxMultiThread.h"
#include "nuke/fnOfxExtensions.h"
#include "ofxMessage.h"
#include "ofxOpenGLRender.h"
#include "Global/OpenFX/OverlayInteractV2.h"
#include "Global/OpenFX/ParamInteractDrawSuite.h"
#include "Global/OpenFX/NatronDrawSuite.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"

namespace IE = OFX::Host::ImageEffect;
namespace P = OFX::Host::Property;
static std::atomic<unsigned> runtimeSuiteCalls{0};
template<typename... Args>
OfxStatus rejectRuntimeCall(Args...)
{
    ++runtimeSuiteCalls;
    return kOfxStatErrMissingHostFeature;
}
template<typename... Args>
void rejectRuntimeCalls(OfxStatus(* &entry)(Args...))
{
    entry = &rejectRuntimeCall<Args...>;
}
static QString q(const std::string &s)
{
    return QString::fromUtf8(s.c_str());
}
static bool pointer(const P::Set &s, const char *name)
{
    return s.fetchProperty(name) && s.getPointerProperty(name) != nullptr;
}
static void prop(P::Set &s, const char *name, P::TypeEnum type, const char *value)
{
    if (!s.fetchProperty(name)) s.createProperty({name, type, 1, false, value});
}
class Descriptor : public IE::Descriptor
{
public:
    using IE::Descriptor::Descriptor;
    OFX::Host::Param::Descriptor *paramDefine(const char *type, const char *name) override
    {
        auto p = IE::Descriptor::paramDefine(type, name);
        prop(p->getProperties(), kNatronOfxParamPropInteractDrawSuiteV1, P::ePointer, nullptr);
        prop(p->getProperties(), kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1, P::ePointer, nullptr);
        prop(p->getProperties(), kOfxParamPropHasHostOverlayHandle, P::eInt, "0");
        prop(p->getProperties(), kOfxParamPropUseHostOverlayHandle, P::eInt, "0");
        return p;
    }
};
class Host : public IE::Host
{
    OfxDrawSuiteV1 draw{};
    NatronOfxDrawSuiteV1 native{};
    NatronOfxVulkanImageSuiteV1 images{};
    NatronOfxVulkanRenderSuiteV1 rendering{};
    NatronOfxVulkanShaderSuiteV1 shaders{};
    NatronOfxVulkanShaderSuiteV2 shadersV2{};
public:
    explicit Host(bool legacyCustom)
    {
        // Describe may test suite availability; every runtime call is rejected
        // and recorded. No loader, device, image or shader is initialized.
        rejectRuntimeCalls(images.createContext);
        rejectRuntimeCalls(images.releaseContext);
        rejectRuntimeCalls(images.getDeviceInfo);
        rejectRuntimeCalls(images.createImage);
        rejectRuntimeCalls(images.releaseImage);
        rejectRuntimeCalls(images.upload);
        rejectRuntimeCalls(images.download);
        rejectRuntimeCalls(images.copy);
        rejectRuntimeCalls(images.transform);
        rejectRuntimeCalls(images.mix);
        rejectRuntimeCalls(images.restoreChannels);
        rejectRuntimeCalls(rendering.acquireImage);
        rejectRuntimeCalls(rendering.getTransferCounts);
        rejectRuntimeCalls(shaders.createProgram);
        rejectRuntimeCalls(shaders.releaseProgram);
        rejectRuntimeCalls(shaders.render);
        rejectRuntimeCalls(shaders.getResources);
        rejectRuntimeCalls(shadersV2.createProgram);
        rejectRuntimeCalls(shadersV2.releaseProgram);
        rejectRuntimeCalls(shadersV2.render);
        rejectRuntimeCalls(shadersV2.getResources);
        auto &s = getProperties();
        s.setStringProperty(kOfxPropName, "fr.inria.Natron");
        s.setStringProperty(kOfxPropLabel, "Natron");
        prop(s, kOfxImageEffectPropOpenGLRenderSupported, P::eString, "false");
        s.setIntProperty(kOfxPropAPIVersion, 1, 0);
        s.setIntProperty(kOfxPropAPIVersion, 5, 1);
        s.setIntProperty(kOfxPropVersion, 2, 0);
        s.setIntProperty(kOfxPropVersion, 6, 1);
        s.setIntProperty(kOfxImageEffectHostPropIsBackground, 0);

        for (auto name : {
                 kOfxImageEffectPropSupportsOverlays, kOfxImageEffectPropSupportsMultiResolution,
                 kOfxImageEffectPropSupportsTiles, kOfxImageEffectPropTemporalClipAccess,
                 kOfxImageEffectPropSupportsMultipleClipDepths, kOfxImageEffectPropSupportsMultipleClipPARs,
                 kOfxImageEffectPropSetableFrameRate, kOfxParamHostPropSupportsStringAnimation,
                 kOfxParamHostPropSupportsChoiceAnimation, kOfxParamHostPropSupportsBooleanAnimation,
                 kOfxParamHostPropSupportsCustomAnimation, kNatronOfxHostIsNatron
             }) s.setIntProperty(name, 1);
        s.setIntProperty(kOfxParamHostPropSupportsCustomInteract, legacyCustom ? 1 : 0);
        s.setIntProperty(kOfxParamHostPropSupportsParametricAnimation, 0);
        s.setStringProperty(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
        s.setStringProperty(kOfxImageEffectPropSupportedComponents, kOfxImageComponentAlpha, 1);
        s.setStringProperty(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGB, 2);
        s.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);
        s.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthShort, 1);
        s.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 2);
        prop(s, "OfxImageEffectPropOpenGLRenderSupported", P::eString, "false");
        prop(s, kNatronOfxParamHostPropSupportsDrawSuiteInteract, P::eInt, "1");
        prop(s, kNatronOfxParamHostPropSupportsDrawSuiteParametricBackground, P::eInt, "1");
        prop(s, "OfxParamHostPropSupportsParametricParameter", P::eInt, "1");
        s.setIntProperty(kNatronOfxParamHostPropSupportsDynamicChoices, 1);
        s.setIntProperty(kNatronOfxParamPropChoiceCascading, 1);
        s.setIntProperty(kFnOfxImageEffectPropMultiPlanar, 1);
    }
    void initDescriptor(IE::Descriptor *d) override
    {
        prop(d->getProps(), kOfxImageEffectPropOpenGLRenderSupported, P::eString, "false");
        prop(d->getProps(), kOfxImageEffectPluginPropOverlayInteractV2, P::ePointer, nullptr);
        prop(d->getProps(), kNatronOfxImageEffectPropVulkanRenderSupported, P::eInt, "0");
        prop(d->getProps(), kNatronOfxImageEffectPropVulkanRenderRequired, P::eInt, "0");
    }
    const void *fetchSuite(const char *name, int version) override
    {
        if (!strcmp(name, kNatronOfxVulkanImageSuite)) {
            return version == 1 ? &images : nullptr;
        }

        if (!strcmp(name, kNatronOfxVulkanRenderSuite)) {
            return version == 1 ? &rendering : nullptr;
        }

        if (!strcmp(name, kNatronOfxVulkanShaderSuite)) {
            if (version == 1) {
                return &shaders;
            }

            return version == 2 ? &shadersV2 : nullptr;
        }

        // Describe only: no drawing callbacks are called and no graphics context is created.
        if (version == 1 && !strcmp(name, kOfxDrawSuite)) {
            return &draw;
        }

        if (version == 1 && !strcmp(name, kNatronOfxDrawSuite)) {
            return &native;
        }

        if (!strcmp(name, "OfxImageEffectOpenGLRenderSuite")) {
            return nullptr;
        }

        return IE::Host::fetchSuite(name, version);
    }
    IE::Instance *newInstance(void *, IE::ImageEffectPlugin *, IE::Descriptor &, const std::string &) override
    {
        return nullptr;
    }
    IE::Descriptor *makeDescriptor(IE::ImageEffectPlugin *p) override
    {
        return new Descriptor(p);
    }
    IE::Descriptor *makeDescriptor(const IE::Descriptor &d, IE::ImageEffectPlugin *p) override
    {
        return new Descriptor(d, p);
    }
    IE::Descriptor *makeDescriptor(const std::string &s, IE::ImageEffectPlugin *p) override
    {
        return new Descriptor(s, p);
    }
    OfxStatus vmessage(const char *type, const char *, const char *fmt, va_list ap) override
    {
        std::fprintf(stderr, "%s: ", type);
        std::vfprintf(stderr, fmt, ap);
        std::fputc('\n', stderr);
        return strcmp(type, kOfxMessageQuestion) ? kOfxStatOK : kOfxStatReplyNo;
    }
    OfxStatus setPersistentMessage(const char *t, const char *id, const char *f, va_list a) override
    {
        return vmessage(t, id, f, a);
    }
    OfxStatus clearPersistentMessage() override
    {
        return kOfxStatOK;
    }
    OfxStatus multiThread(OfxThreadFunctionV1 f, unsigned int n, void *a) override
    {
        for (unsigned int i = 0; i < n; ++i) {
            f(i, n, a);
        }

        return kOfxStatOK;
    }
    OfxStatus multiThreadNumCPUS(unsigned int *n) const override
    {
        *n = 1;
        return kOfxStatOK;
    }
    OfxStatus multiThreadIndex(unsigned int *i) const override
    {
        *i = 0;
        return kOfxStatOK;
    }
    int multiThreadIsSpawnedThread() const override
    {
        return 0;
    }
    OfxStatus mutexCreate(OfxMutexHandle *m, int locks) override
    {
        auto p = new std::recursive_mutex;

        for (int i = 0; i < locks; ++i) {
            p->lock();
        }

        *m = reinterpret_cast<OfxMutexHandle>(p);
        return kOfxStatOK;
    }
    OfxStatus mutexDestroy(OfxMutexHandle m) override
    {
        delete reinterpret_cast<std::recursive_mutex *>(m);
        return kOfxStatOK;
    }
    OfxStatus mutexLock(OfxMutexHandle m) override
    {
        reinterpret_cast<std::recursive_mutex *>(m)->lock();
        return kOfxStatOK;
    }
    OfxStatus mutexUnLock(OfxMutexHandle m) override
    {
        reinterpret_cast<std::recursive_mutex *>(m)->unlock();
        return kOfxStatOK;
    }
    OfxStatus mutexTryLock(OfxMutexHandle m) override
    {
        return reinterpret_cast<std::recursive_mutex *>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
    }
    OfxStatus requestDialog(OfxImageEffectHandle, OfxPropertySetHandle, void *) override
    {
        return kOfxStatFailed;
    }
    OfxStatus notifyRedrawPending(OfxImageEffectHandle, OfxPropertySetHandle) override
    {
        return kOfxStatReplyDefault;
    }
};
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    QCoreApplication app(argc, argv);

    if (argc != 4) {
        return 2;
    }

    Host host(std::string(argv[3]) == "legacy-discovery");
    OFX::Host::PluginCache::useStdOFXPluginsLocation(false);
    auto cache = OFX::Host::PluginCache::getPluginCache();
    cache->addFileToPath(argv[1]);
    IE::PluginCache effects(&host);
    cache->registerAPICache(kOfxImageEffectPluginApi, 1, 1, &effects);
    cache->scanPluginFiles();
    QJsonArray rows, failures;
    int index = 0;

    for (auto p : effects.getPlugins()) {
        if (!(++index % 50)) {
            std::fprintf(stderr, "Inspected %d plugins\n", index);
        }

        try {
            if (!p->getPluginHandle()) {
                failures.append(q(p->getIdentifier()) + ": Load/Describe failed");
                continue;
            }

            QJsonObject row{{"id", q(p->getIdentifier())}, {"label", q(p->getDescriptor().getLabel())},
                {"major", p->getVersionMajor()}, {"minor", p->getVersionMinor()},
                {"binary", q(p->getBinary()->getFilePath())}, {"bundle", q(p->getBinary()->getBundlePath())}};
            QJsonArray contexts;

            for (const auto &name : p->getContexts()) {
                auto d = p->getContext(name);

                if (!d) {
                    failures.append(q(p->getIdentifier()) + ": DescribeInContext failed: " + q(name));
                    continue;
                }

                QJsonArray params;

                for (auto param : d->getParamList()) {
                    const auto &s = param->getProperties();
                    bool legacy = pointer(s, kOfxParamPropInteractV1);
                    bool native = pointer(s, kNatronOfxParamPropInteractDrawSuiteV1);
                    bool legacyBg = pointer(s, kOfxParamPropParametricInteractBackground);
                    bool nativeBg = pointer(s, kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1);

                    if (legacy || native || legacyBg || nativeBg) params.append(QJsonObject{
                        {"name", q(param->getName())}, {"type", q(param->getType())},
                        {"legacyCustom", legacy}, {"drawSuiteCustom", native},
                        {"legacyBackground", legacyBg}, {"drawSuiteBackground", nativeBg}});
                }

                contexts.append(QJsonObject{{"context", q(name)},
                    {"retiredOpenGLRendering", q(d->getProps().getStringProperty(kOfxImageEffectPropOpenGLRenderSupported))},
                    {"vulkanRendering", d->getProps().getIntProperty(kNatronOfxImageEffectPropVulkanRenderSupported) == 1},
                    {"vulkanRequired", d->getProps().getIntProperty(kNatronOfxImageEffectPropVulkanRenderRequired) == 1},
                    {"legacyOverlay", pointer(d->getProps(), kOfxImageEffectPluginPropOverlayInteractV1)},
                    {"drawSuiteOverlay", pointer(d->getProps(), kOfxImageEffectPluginPropOverlayInteractV2)},
                    {"params", params}});
            }

            row["contexts"] = contexts;
            auto module = GetModuleHandleA(p->getBinary()->getFilePath().c_str());

            for (auto symbol : {
                     "OfxDrawSuiteSmokeCounter", "OfxParamDrawSuiteSmokeCounter", "OfxCurveDrawSuiteSmokeCounter"
                 }) {
                auto counter = reinterpret_cast<unsigned long long (*)(int)>(GetProcAddress(module, symbol));

                if (counter) {
                    row["fixtureLegacyCalls"] = double(counter(0));
                }
            }
            rows.append(row);
        } catch (const std::exception &e) {
            failures.append(q(p->getIdentifier()) + ": " + e.what());
        }
    }

    if (runtimeSuiteCalls) {
        failures.append("A plug-in attempted to invoke a runtime Vulkan suite during descriptor inspection.");
    }

    QJsonObject report{{"mode", argv[3]}, {"pluginVersions", rows}, {"failures", failures},
        {"runtimeVulkanSuiteCalls", int(runtimeSuiteCalls)},
        {"scope", "Effect Load/Describe/DescribeInContext declarations only; no effect or interact instances, rendering or GL contexts."}};
    QFile file(QString::fromLocal8Bit(argv[2]));

    if (!file.open(QIODevice::WriteOnly)) {
        return 3;
    }

    file.write(QJsonDocument(report).toJson());
    file.close();
    std::fprintf(stderr, "Recorded %d plugin versions, %d failures\n", int(rows.size()), int(failures.size()));
    OFX::Host::PluginCache::clearPluginCache();
    return failures.empty() ? 0 : 1;
}
