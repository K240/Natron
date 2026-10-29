// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Compositing.h"
#include "Gpu/UsdSceneBridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace Natron::Gpu;
using namespace Natron::Gpu::Cards;
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::shared_ptr<Texture> solid(std::array<float, 4> color)
{
    auto t = std::make_shared<Texture>();
    t->width = t->height = 1;
    t->pixels.assign(color.begin(), color.end());
    return t;
}
double sumAlpha(const std::vector<float>& p)
{
    double sum = 0;
    for (size_t i = 3; i < p.size(); i += 4)
        sum += p[i];
    return sum;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        Settings s;
        s.width = 96;
        s.height = 64;
        s.samples = 1;
        s.transparentBackground = true;
        Camera camera;
        camera.orthographic = true;
        camera.orthoWidth = 4;
        Card ground;
        ground.width = 4;
        ground.height = 3;
        ground.ownerId = "catcher";
        ground.compositing.role = GeometryRole::ShadowCatcher;
        Card cube;
        cube.width = .6;
        cube.height = .8;
        cube.transform = Matrix::transform({ .65, 0, 1 }, {});
        cube.texture = solid({ .8f, .1f, .03f, 1 });
        cube.ownerId = "caster";
        Light light;
        light.type = LightType::Distant;
        light.transform = Matrix::transform({}, { 0, -45, 0 });
        s.lights = { light };
        auto shadowSettings = s;
        shadowSettings.pass = RenderPass::Shadow;
        auto plain = renderCpu({ ground }, camera, shadowSettings);
        require(sumAlpha(plain) == 0, "Empty receiver fabricated a shadow");
        auto shadow = renderCpu({ ground, cube }, camera, shadowSettings);
        require(sumAlpha(shadow) > 100, "CG did not cast a shadow onto the receiver");
        auto combined = ground;
        combined.compositing.role = GeometryRole::ShadowReflectionCatcher;
        require(renderCpu({ combined, cube }, camera, shadowSettings) == shadow,
            "Enabling reflections changed independent shadow reception");
        auto beauty = renderCpu({ ground, cube }, camera, s);
        require(
            sumAlpha(beauty) > 100 && sumAlpha(beauty) < 500, "Catcher became visible in foreground beauty");
        auto noCaster = cube;
        noCaster.compositing.shadowVisible = false;
        require(sumAlpha(renderCpu({ ground, noCaster }, camera, shadowSettings)) == 0,
            "Shadow visibility was ignored");
        auto half = ground;
        half.compositing.shadowDensity = .5;
        half.compositing.shadowTint = { .1, .03, .01 };
        auto halfShadow = renderCpu({ half, cube }, camera, shadowSettings);
        require(std::abs(sumAlpha(halfShadow) - sumAlpha(shadow) * .5) < 1e-4,
            "Shadow density is not independent");
        for (size_t i = 0; i < halfShadow.size(); i += 4)
            require(
                std::abs(halfShadow[i] - .1 * halfShadow[i + 3]) < 1e-6, "Shadow tint is not premultiplied");
        const float f[] { .1f, .05f, .02f, .25f }, sh[] { .02f, .01f, 0, .5f }, bg[] { .4f, .2f, .1f, .5f };
        auto comp = compositePlatePixel(f, sh, bg);
        require(std::abs(comp[3] - .625) < 1e-6, "Shadow polluted plate coverage");
        require(std::abs(comp[0] - (.1 + .75 * (.4 * .5 + .02 * .5))) < 1e-6,
            "Premultiplied colored shadow composition is incorrect");
        Card proxy;
        proxy.ownerId = "proxy";
        proxy.width = .35;
        proxy.height = .6;
        proxy.transform = Matrix::transform({ .65, 0, 2 }, {});
        proxy.compositing.role = GeometryRole::Occluder;
        proxy.compositing.shadowVisible = false;
        const auto hidden = renderCpu({ ground, cube, proxy }, camera, s);
        require(sumAlpha(hidden) < sumAlpha(beauty) - 50, "Front proxy failed to occlude CG");
        auto matteSettings = s;
        matteSettings.pass = RenderPass::Occlusion;
        auto matte = renderCpu({ ground, cube, proxy }, camera, matteSettings);
        require(sumAlpha(matte) > 50, "Occluder matte was not produced");
        proxy.transform.m[14] = -.5;
        require(renderCpu({ ground, cube, proxy }, camera, s) == beauty, "Back proxy occluded foreground CG");
        proxy.transform.m[14] = 2;
        auto noLights = shadowSettings;
        noLights.lights.clear();
        require(sumAlpha(renderCpu({ ground, cube }, camera, noLights)) == 0,
            "Unlit receiver fabricated shadows");
        if (argc > 1 && std::string(argv[1]) == "--gpu")
        {
            InstanceOptions options;
            options.validation = true;
            auto instance = VulkanInstance::create(options);
            auto device = VulkanDevice::create(instance);
            Renderer renderer(device);
            auto both = half;
            both.compositing.role = GeometryRole::ShadowReflectionCatcher;
            const std::vector<Card> cards { both, cube, proxy };
            for (auto pass :
                { RenderPass::Color, RenderPass::Depth, RenderPass::Shadow, RenderPass::Occlusion })
            {
                auto settings = s;
                settings.pass = pass;
                auto cpu = renderCpu(cards, camera, settings), gpu = renderer.render(cards, camera, settings);
                double error = 0;
                for (size_t i = 0; i < cpu.size(); ++i)
                    error = std::max(error, std::abs(double(cpu[i]) - gpu[i]));
                std::cout << "CPU/Vulkan pass " << int(pass) << " max difference " << error << '\n';
                require(error < .002, "Vulkan compositing differs from CPU");
            }
            require(instance->validationErrorCount() == 0, "Vulkan validation failed");
        }
        if (argc > 1 && std::string(argv[1]) == "--hdnvp")
        {
            UsdSceneBridge bridge;
            HdNvpSettings quality;
            quality.samples = 32;
            quality.bounces = 2;
            quality.denoiser = HdNvpDenoiser::None;
            auto frame = bridge.render({ ground, cube, proxy }, camera, s, quality);
            require(sumAlpha(frame.shadow) > 20 && !frame.occlusion.empty(),
                "USD renderer did not return shadow/occluder outputs");
            require(sumAlpha(frame.color) < sumAlpha(beauty), "USD holdout did not cut foreground coverage");
            auto original = bridge.sceneValue();
            const auto targets = original->compositingTargets();
            require(targets.size() == 3, "Published USD roles disappeared");
            auto moved = proxy;
            moved.transform.m[14] = -.5;
            auto back = bridge.render({ ground, cube, moved }, camera, s, quality);
            require(sumAlpha(back.color) > sumAlpha(frame.color) + 30,
                "USD depth ordering ignored the moved proxy");
            auto unshadowed = bridge.render({ ground, noCaster, moved }, camera, s, quality);
            require(sumAlpha(unshadowed.shadow) < sumAlpha(back.shadow) * .1,
                "USD shadow visibility was ignored");
            require(
                original->compositingTargets().size() == 3, "Compositing probes mutated a published stage");
            Card left = ground, right = ground;
            left.width = right.width = 2;
            left.ownerId = "left";
            right.ownerId = "right";
            left.transform.m[12] = -1;
            right.transform.m[12] = 1;
            auto grouped = bridge.render({ left, right, cube }, camera, s, quality);
            require(sumAlpha(grouped.shadow) > 20, "Grouped receiver meshes lost their shadows");
            UsdSceneBridge sourceBridge;
            sourceBridge.exportScene({ cube }, camera, s);
            QTemporaryDir directory;
            require(directory.isValid(), "No private source fixture directory");
            const auto filename = directory.filePath(QStringLiteral("proxy.usda"));
            sourceBridge.sceneValue()->saveAssembly(filename.toStdString());
            UsdSceneSource source;
            source.filename = filename.toStdString();
            source.primPath = "/Natron/Geometry/n_caster";
            source.mountPath = "/Natron/Assets/proxy";
            source.ownerId = "usd-proxy";
            auto settings = s;
            UsdPrimEdit edit;
            edit.kind = UsdPrimEdit::Kind::Compositing;
            edit.identity = "proxy-role";
            edit.paths = { source.mountPath };
            edit.role = GeometryRole::Occluder;
            settings.edits = { edit };
            auto asProxy = bridge.render({ ground }, camera, settings, quality, {}, { source });
            require(!asProxy.occlusion.empty()
                    && std::accumulate(asProxy.occlusion.begin(), asProxy.occlusion.end(), 0.) > 50,
                "Referenced USD subtree did not become an occluder");
            auto targetsWithProxy = bridge.sceneValue()->compositingTargets();
            require(std::any_of(targetsWithProxy.begin(), targetsWithProxy.end(),
                        [](const auto& t)
                        {
                            return t.path.find("/Assets/proxy/") != std::string::npos
                                && t.value.role == GeometryRole::Occluder;
                        }),
                "Source primvar defaults overrode the downstream role");
            int polls = 0;
            bool cancelled = false;
            try
            {
                bridge.render({ ground, cube }, camera, s, quality, [&] { return ++polls >= 3; });
            }
            catch (const std::runtime_error& e)
            {
                cancelled = std::string(e.what()).find("cancel") != std::string::npos;
            }
            require(cancelled, "Compositing probes did not cooperate with cancellation");
            const auto exported = bridge.exportScene({ ground, cube }, camera, s);
            QFile geometry(
                QFileInfo(QString::fromStdString(exported)).dir().filePath(QStringLiteral("geometry.usda")));
            require(geometry.open(QIODevice::ReadOnly) && !geometry.readAll().contains("CompositingProbe"),
                "Cancelled probe leaked into the public render assembly");
        }
        std::cout << "Compositing passed: differential shadows, density/tint, transparent plate alpha, "
                     "depth-ordered holdout and independent mattes\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
