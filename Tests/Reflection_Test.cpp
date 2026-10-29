// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Compositing.h"
#include "Gpu/UsdSceneBridge.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace Natron::Gpu;
using namespace Natron::Gpu::Cards;
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
double energy(const std::vector<float>& p)
{
    double r = 0;
    for (size_t i = 0; i < p.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            r += std::abs(p[i + c]);
    return r;
}
double difference(const std::vector<float>& a, const std::vector<float>& b)
{
    require(a.size() == b.size(), "Image sizes differ");
    double r = 0;
    for (size_t i = 0; i < a.size(); ++i)
        r += std::abs(a[i] - b[i]);
    return r;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        const bool usd = argc > 1 && std::string(argv[1]) == "--hdnvp";
        Settings s;
        s.width = 96;
        s.height = 64;
        s.samples = 1;
        s.pass = RenderPass::Reflection;
        s.transparentBackground = true;
        Camera camera;
        camera.transform = Matrix::transform({ 0, 1.5, 4 }, { -20, 0, 0 });
        Card floor;
        floor.ownerId = "floor";
        floor.width = 6;
        floor.height = 6;
        floor.transform = Matrix::transform({}, { -90, 0, 0 });
        floor.compositing.role = GeometryRole::ReflectionCatcher;
        floor.compositing.reflectionStrength = 1;
        floor.compositing.reflectionRoughness = 0;
        floor.compositing.reflectionVisible = false;
        Card model;
        model.ownerId = "model";
        model.width = .8;
        model.height = 1;
        model.transform = Matrix::transform({ 0, .65, 0 }, {});
        model.material.lit = true;
        model.material.baseColor = { 0, 0, 0, 1 };
        model.material.emission = { 2, .1, .01 };
        UsdSceneBridge bridge;
        HdNvpSettings quality;
        quality.samples = 64;
        quality.bounces = 2;
        quality.denoiser = HdNvpDenoiser::None;
        auto render = [&](const std::vector<Card>& cards)
        {
            if (usd)
            {
                auto f = bridge.render(cards, camera, s, quality);
                return f.reflection.empty() ? std::vector<float>(size_t(s.width) * s.height * 4)
                                            : f.reflection;
            }
            return renderCpu(cards, camera, s);
        };
        const auto sharp = render({ floor, model });
        const auto sharpEnergy = energy(sharp);
        std::cout << "Sharp reflection energy " << sharpEnergy << '\n';
        require(sharpEnergy > 10, "Model is missing from the mirror reflection");
        auto half = floor;
        half.compositing.reflectionStrength = .5;
        require(std::abs(energy(render({ half, model })) - sharpEnergy * .5) < sharpEnergy * .001,
            "Reflection strength is not linear or cache stayed stale");
        half.compositing.reflectionStrength = 0;
        require(energy(render({ half, model })) == 0, "Zero reflection strength changes the plate");
        auto invisible = model;
        invisible.compositing.reflectionVisible = false;
        require(
            energy(render({ floor, invisible })) < sharpEnergy * .001, "Secondary visibility was ignored");
        invisible = model;
        invisible.compositing.cameraVisible = false;
        require(std::abs(energy(render({ floor, invisible })) - sharpEnergy) < sharpEnergy * .001,
            "Camera-hidden model lost its reflection");
        auto rough = floor;
        rough.compositing.reflectionRoughness = .65;
        const auto blurred = render({ rough, model });
        require(
            difference(sharp, blurred) > sharpEnergy * .2, "Roughness did not change the reflected image");
        require(energy(blurred) > 1, "Rough reflection disappeared");
        Light env;
        env.type = LightType::Environment;
        env.environmentSamples = 16;
        env.shadows = false;
        s.lights = { env };
        require(energy(render({ floor })) < 1e-5, "An empty catcher added environment reflections twice");
        if (!usd)
        {
            Card empty;
            empty.transform = model.transform;
            require(energy(render({ floor, empty })) == 0,
                "An empty native Card appeared as phantom CG in reflections");
        }
        auto dark = model;
        dark.material.emission = {};
        const auto darkReflection = render({ floor, dark });
        require(*std::min_element(darkReflection.begin(), darkReflection.end()) < -.05f,
            "Dark model cannot replace a bright environment reflection");
        Card proxy = floor;
        proxy.ownerId = "proxy";
        proxy.compositing.role = GeometryRole::Occluder;
        proxy.transform = Matrix::transform({ 0, .05, 0 }, { -90, 0, 0 });
        require(energy(render({ floor, model, proxy })) < 1e-5,
            "Foreground plate proxy did not clip reflections");
        if (usd)
        {
            const auto value = bridge.sceneValue();
            const auto targets = value->compositingTargets();
            require(std::any_of(targets.begin(), targets.end(),
                        [](const auto& t)
                        {
                            return catchesReflections(t.value.role) && t.value.reflectionStrength == 1
                                && t.value.reflectionRoughness == 0;
                        }),
                "Published USD reflection controls were lost");
            UsdSceneBridge asset;
            asset.exportScene({ floor }, camera, s);
            QTemporaryDir directory;
            require(directory.isValid(), "No source fixture directory");
            const auto filename = directory.filePath(QStringLiteral("floor.usda")).toStdString();
            asset.sceneValue()->saveAssembly(filename);
            UsdSceneSource source;
            source.filename = filename;
            source.primPath = "/Natron/Geometry/n_floor";
            source.mountPath = "/Natron/Assets/floor";
            source.ownerId = "sourceFloor";
            UsdPrimEdit edit;
            edit.kind = UsdPrimEdit::Kind::Compositing;
            edit.paths = { source.mountPath };
            edit.role = GeometryRole::ShadowReflectionCatcher;
            edit.reflectionStrength = .5;
            edit.reflectionRoughness = .6;
            s.edits = { edit };
            const auto sourced = bridge.render({ model }, camera, s, quality, {}, { source });
            require(energy(sourced.reflection) > 1, "Referenced receiver did not reflect CG");
            const auto edited = bridge.sceneValue()->compositingTargets();
            require(std::any_of(edited.begin(), edited.end(),
                        [](const auto& t)
                        {
                            return t.value.role == GeometryRole::ShadowReflectionCatcher
                                && t.value.reflectionStrength == .5
                                && std::abs(t.value.reflectionRoughness - .6) < 1e-5;
                        }),
                "Referenced Mesh defaults overrode reflection controls");
            require(value->compositingTargets().size() == targets.size(),
                "Reflection probes mutated the published branch");
            // 3D Viewer inspection must survive every renderer-only probe.
            // The shot camera remains published; the view camera is private.
            s.edits.clear();
            auto shotCamera = camera;
            shotCamera.transform.m[12] = 2;
            s.stageCamera = std::make_shared<Camera>(shotCamera);
            const auto plainView = bridge.render({ model }, camera, s, quality);
            require(energy(plainView.color) > 1, "Inspection camera failed without catchers");
            auto both = floor;
            both.compositing.role = GeometryRole::ShadowReflectionCatcher;
            proxy.width = proxy.height = .1;
            const auto view = bridge.render({ both, model, proxy }, camera, s, quality);
            require(energy(view.reflection) > 10 && !view.shadow.empty() && !view.occlusion.empty(),
                "Inspection camera failed in shadow/reflection/occluder probes");
            const auto published = bridge.sceneValue();
            require(std::abs(published->camera("/Natron/Camera").transform.m[12] - 2) < 1e-6,
                "Inspection replaced the published shot camera");
            bool privateCamera = false;
            try
            {
                published->camera("/Natron/InspectionCamera");
            }
            catch (const std::runtime_error&)
            {
                privateCamera = true;
            }
            require(privateCamera, "Inspection camera leaked into the published USD branch");
            auto movedView = camera;
            movedView.transform.m[12] += .2;
            const auto moved = bridge.render({ both, model, proxy }, movedView, s, quality);
            require(difference(view.reflection, moved.reflection) > 1,
                "Camera-only inspection updates kept stale probe transforms");
        }
        else if (argc > 1 && std::string(argv[1]) == "--gpu")
        {
            InstanceOptions options;
            options.validation = true;
            auto instance = VulkanInstance::create(options);
            auto device = VulkanDevice::create(instance);
            Renderer renderer(device);
            require(difference(
                        renderer.render({ floor, model }, camera, s), renderCpu({ floor, model }, camera, s))
                    == 0,
                "Native reflection fallback differs");
            require(instance->validationErrorCount() == 0, "Vulkan validation failed");
        }
        const float f[4] = { .1f, .05f, .02f, .25f }, shadow[4] = { 0, 0, 0, .4f },
                    plate[4] = { .4f, .2f, .1f, .5f }, r[4] = { -.1f, .2f, .3f, 0 };
        const auto out = compositePlatePixel(f, shadow, plate, r);
        require(std::abs(out[0] - (.1 + .75 * (.4 * .6 - .1 * .5))) < 1e-6 && std::abs(out[3] - .625) < 1e-6,
            "Signed reflection composition changed coverage or ignored premultiplication");
        const float transparent[4] = {};
        const auto clear = compositePlatePixel(transparent, shadow, transparent, r);
        require(clear == std::array<float, 4> {}, "Reflection polluted transparent plate pixels");
        std::cout << "Reflection passed: strength, roughness, independent visibility, differential "
                     "environment, dark reflections, occlusion, plate alpha\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
