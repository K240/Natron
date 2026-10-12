// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Qt/VulkanCanvas.h"
#include "Gpu/ViewerRenderer.h"
#include "ModuleAudit.h"
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QScreen>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#endif

using namespace Natron::Gpu;
namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
QImage capture(VulkanCanvas* canvas, bool inverted, const QString& path = {})
{
    const auto image = canvas->frameImage();
    require(image.width() == std::lround(canvas->width() * canvas->devicePixelRatio()) &&
            image.height() == std::lround(canvas->height() * canvas->devicePixelRatio()), "Vulkan frame has incorrect DPI dimensions.");
    require(!image.isNull(), "Could not capture the native Vulkan surface.");

    if (!path.isEmpty()) {
        require(image.save(path), "Could not save Vulkan screenshot.");
    }

    // Validate actual presented pixels, independently of source-image readback.
    for (double x : {
             0.27, 0.47, 0.73
         }) {
        for (double y : {
                 0.27, 0.47, 0.73
             }) {
            const auto pixel = image.pixelColor(static_cast<int>(x * image.width()), static_cast<int>(y * image.height()));
            const double r = inverted ? 1 - x : x;
            const double g = inverted ? 1 - y : y;
            const int sourceX = static_cast<int>((std::floor(x * image.width()) + 0.5) / image.width() * 3840);
            const int sourceY = static_cast<int>((std::floor(y * image.height()) + 0.5) / image.height() * 2160);
            const double blue = ((sourceX / 128 + sourceY / 128) % 2) ? 0.8 : 0.2;
            const double b = inverted ? 1 - blue : blue;

            if (std::abs(pixel.redF() - r) >= 0.035 || std::abs(pixel.greenF() - g) >= 0.035) {
                std::cerr << "Capture " << image.width() << 'x' << image.height() << " at " << x << ',' << y
                          << ": " << pixel.redF() << ',' << pixel.greenF() << " expected " << r << ',' << g << std::endl;
            }

            require(std::abs(pixel.redF() - r) < 0.035 && std::abs(pixel.greenF() - g) < 0.035 &&
                    std::abs(pixel.blueF() - b) < 0.025 && pixel.alpha() == 255,
                    "Presented Vulkan pixels differ from the CPU reference.");
        }
    }
    return image;
}
struct ViewportScene {
    ViewportScene(const std::shared_ptr<VulkanDevice> &device, std::shared_ptr<VulkanImage> a, std::shared_ptr<VulkanImage> b)
        : renderer(device), source(std::move(a)), result(std::move(b))
    {
        for (size_t i = 0; i < 2; ++i) {
            inputs[i].image = i == 0 ? source.get() : result.get();
            inputs[i].bounds = inputs[i].clip = inputs[i].format = {0, 0, 3840, 2160};
        }

        settings.viewport = {0, 0, 3840, 2160};
        settings.wipe = {1920, 1080, 0, 1};
        settings.operation = ViewerOperator::WipeUnder;
    }
    ViewerRenderer renderer;
    std::shared_ptr<VulkanImage> source, result;
    std::array<ViewerInput, 2> inputs;
    ViewerSettings settings;
};
void captureViewport(VulkanCanvas* canvas, const ViewportScene& scene, const QString& path)
{
    const auto image = canvas->frameImage();
    require(!image.isNull() && image.width() == std::lround(canvas->width() * canvas->devicePixelRatio()) &&
            image.height() == std::lround(canvas->height() * canvas->devicePixelRatio()), "Viewport target has incorrect DPI dimensions.");
    require(image.save(path), "Could not save viewport screenshot.");

    for (double x : {
             0.033, 0.273, 0.473, 0.737, 0.967
         }) {
        for (double y : {
                 0.033, 0.273, 0.473, 0.737, 0.967
             }) {
            const int px = static_cast<int>(x * image.width()), py = static_cast<int>(y * image.height());
            const auto& viewport = scene.settings.viewport;
            const double cx = viewport[0] + (px + 0.5) / image.width() * (viewport[2] - viewport[0]);
            const double cy = viewport[3] - (py + 0.5) / image.height() * (viewport[3] - viewport[1]);
            auto expected = scene.settings.background;

            if (cx >= 0 && cx < 3840 && cy >= 0 && cy < 2160) {
                const auto sx = static_cast<int>(std::floor(cx)), sy = static_cast<int>(std::floor(cy));
                expected = {sx / 3839.0f, sy / 2159.0f, ((sx / 128 + sy / 128) % 2) ? 0.8f : 0.2f, 1};
                const bool inverted = scene.settings.operation == ViewerOperator::WipeUnder && cx >= scene.settings.wipe[0];

                for (int c = 0; c < 3; ++c) {
                    if (inverted) {
                        expected[c] = 1 - expected[c];
                    }

                    if (scene.inputs[0].lut == ViewerLut::SRGB) {
                        expected[c] = expected[c] <= 0.0031308f ? expected[c] * 12.92f :
                                      static_cast<float>(1.055 * std::pow(expected[c], 1 / 2.4) - 0.055);
                    }
                }
            }

            const auto actual = image.pixelColor(px, py);

            if (std::abs(actual.redF() - expected[0]) > 0.008 || std::abs(actual.greenF() - expected[1]) > 0.008 ||
                std::abs(actual.blueF() - expected[2]) > 0.008 || actual.alpha() != 255) {
                std::cerr << "Viewport " << px << ',' << py << " canonical " << cx << ',' << cy << ": "
                          << actual.redF() << ',' << actual.greenF() << ',' << actual.blueF() << " expected "
                          << expected[0] << ',' << expected[1] << ',' << expected[2] << std::endl;
                throw std::runtime_error("Presented viewport pixels differ from the CPU reference.");
            }
        }
    }
}
}

int main(int argc, char** argv)
{
    ModuleAudit moduleAudit;
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    try {
        int index = -1;
        QString output = QDir::currentPath();

        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);

            if (option == "--gpu" && i + 1 < argc) {
                index = std::stoi(argv[++i]);
            } else if (option == "--output" && i + 1 < argc) {
                output = QString::fromUtf8(argv[++i]);
            } else {
                throw std::invalid_argument("Usage: NatronVulkanViewer [--gpu index] [--output directory]");
            }
        }

        require(QDir().mkpath(output), "Could not create Viewer report directory.");
        auto headlessInstance = VulkanInstance::create();
        auto headlessDevice = VulkanDevice::create(headlessInstance, index);
        std::array<std::future<unsigned>, 4> headlessWorkers;

        for (unsigned worker = 0; worker < headlessWorkers.size(); ++worker) {
            headlessWorkers[worker] = std::async(std::launch::async, [index, worker] {
                unsigned errors = 0;

                for (unsigned iteration = 0; iteration < 4; ++iteration)
                {
                    InstanceOptions options;
                    options.validation = (worker + iteration) % 2 == 0;
                    auto instance = VulkanInstance::create(options);
                    auto device = VulkanDevice::create(instance, index);
                    VulkanImage image(device, 2, 2, VK_FORMAT_R32G32B32A32_SFLOAT);
                    image.clear({.25f, .5f, 1.f, 1.f});
                    const auto bytes = image.download();
                    require(bytes.size() == 16 * sizeof(float), "Headless image transfer size differs.");
                    std::array<float, 16> values;
                    std::memcpy(values.data(), bytes.data(), bytes.size());

                    for (unsigned component = 0; component < values.size(); ++component) {
                        require(values[component] == std::array<float, 4> {.25f, .5f, 1.f, 1.f} [component % 4],
                                "Headless/presentation function dispatch changed pixel values.");
                    }

                    errors += instance->validationErrorCount();
                }
                return errors;
            });
        }

        InstanceOptions displayOptions;
        displayOptions.validation = true;
        displayOptions.availableSurfaceExtensions = true;
        auto displayInstance = VulkanInstance::create(displayOptions);
        auto displayDevice = VulkanDevice::create(displayInstance, index);
        auto instance = QtVulkanInstance::adopt(displayInstance);
        require(!headlessInstance->surfaceEnabled() && displayInstance->surfaceEnabled(), "Headless/display instance extension policy differs.");
        {
            bool rejected = false;

            try {
                VulkanCanvas invalid(instance, index, headlessDevice);
            } catch (const std::runtime_error &) {
                rejected = true;
            }

            require(rejected, "A foreign headless device was accepted for presentation.");
        }
        QJsonObject report;
        report.insert(QString::fromUtf8("deviceCreatedBeforeSurface"), true);
        bool passed = false;
        {
            QMainWindow window;
            window.setWindowTitle(QString::fromUtf8("Natron Vulkan shared-device test"));
            auto* first = new VulkanCanvas(instance, index, displayDevice);
            const auto device = first->device();
            auto* second = new VulkanCanvas(instance, index, device);
            first->setCaptureEnabled(true);
            second->setCaptureEnabled(true);
            auto source = std::make_shared<VulkanImage>(device, 3840, 2160);
            auto result = std::make_shared<VulkanImage>(device, 3840, 2160);
            std::vector<float> pixels(source->byteSize() / sizeof(float));

            for (uint32_t y = 0; y < source->height(); ++y) {
                for (uint32_t x = 0; x < source->width(); ++x) {
                    const auto i = (static_cast<size_t>(y) * source->width() + x) * 4;
                    pixels[i] = static_cast<float>(x) / (source->width() - 1);
                    pixels[i + 1] = static_cast<float>(y) / (source->height() - 1);
                    pixels[i + 2] = ((x / 128 + y / 128) % 2) ? 0.8f : 0.2f;
                    pixels[i + 3] = 1;
                }
            }

            source->upload(pixels.data(), source->byteSize());
            ImageTransform transform(device);
            transform.apply(*source, *result, {-1, -1, -1, 1}, {1, 1, 1, 0});
            first->setImage(source);
            second->setImage(result);
            auto viewportScene = std::make_shared<ViewportScene>(device, source, result);
            auto* dockA = new QDockWidget(QString::fromUtf8("Source"), &window);
            auto* dockB = new QDockWidget(QString::fromUtf8("Vulkan compute output"), &window);
            dockA->setWidget(QWidget::createWindowContainer(first, dockA));
            dockB->setWidget(QWidget::createWindowContainer(second, dockB));
            window.addDockWidget(Qt::LeftDockWidgetArea, dockA);
            window.addDockWidget(Qt::RightDockWidgetArea, dockB);
            window.resize(1000, 500);
            window.show();
            window.raise();
            window.activateWindow();
            QTimer timer;
            int stage = 0;
            uint64_t firstGeneration = 0;
            QObject::connect(&timer, &QTimer::timeout, [&] {
                try
                {
                    require(first->error().empty() && second->error().empty(), "A Vulkan canvas reported an error.");

                    if (stage == 0) {
                        require(first->presentedFrames() && second->presentedFrames(), "The initial Viewer frame was not presented.");
                        capture(first, false, output + QString::fromUtf8("/source-initial.png"));
                        capture(second, true, output + QString::fromUtf8("/compute-initial.png"));
                        firstGeneration = first->swapchainGeneration();
                        window.resize(1200, 650);
                    } else if (stage == 1) {
                        require(first->swapchainGeneration() > firstGeneration, "Resize did not recreate the swapchain.");
                        capture(first, false);
                        dockB->setFloating(true);
                        dockB->resize(480, 320);
                        dockB->move(window.x() + window.width() + 10, window.y());
                    } else if (stage == 2) {
                        capture(second, true).save(output + QString::fromUtf8("/compute-detached.png"));
                        dockB->setFloating(false);
                    } else if (stage == 3) {
                        capture(first, false).save(output + QString::fromUtf8("/source-final.png"));
                        capture(second, true).save(output + QString::fromUtf8("/compute-final.png"));
                        require(first->device() == second->device(), "Viewers did not share the logical device.");
                        second->setFrameRenderer([viewportScene](VulkanImage & target) {
                            viewportScene->renderer.render(viewportScene->inputs, viewportScene->settings, target);
                        });
                    } else if (stage == 4) {
                        captureViewport(second, *viewportScene, output + QString::fromUtf8("/viewport-wipe.png"));
                        viewportScene->settings.viewport = {960, 540, 2880, 1620};
                        second->requestUpdate();
                    } else if (stage == 5) {
                        captureViewport(second, *viewportScene, output + QString::fromUtf8("/viewport-pan-zoom.png"));
                        viewportScene->settings.viewport = {-960, -540, 4800, 2700};
                        viewportScene->settings.operation = ViewerOperator::None;
                        viewportScene->inputs[0].lut = ViewerLut::SRGB;
                        second->requestUpdate();
                    } else if (stage == 6) {
                        captureViewport(second, *viewportScene, output + QString::fromUtf8("/viewport-srgb.png"));
                        firstGeneration = second->swapchainGeneration();
                        dockB->setFloating(true);
                        dockB->resize(540, 380);
                    } else if (stage == 7) {
                        require(second->swapchainGeneration() > firstGeneration, "Viewport resize did not recreate the swapchain.");
                        captureViewport(second, *viewportScene, output + QString::fromUtf8("/viewport-srgb-resized.png"));
                        report.insert(QString::fromUtf8("device"), QString::fromUtf8(device->properties().deviceName));
                        report.insert(QString::fromUtf8("sharedDevice"), true);
                        report.insert(QString::fromUtf8("sourceSize"), QString::fromUtf8("3840x2160 RGBA32F"));
                        report.insert(QString::fromUtf8("sourceFrames"), static_cast<qint64>(first->presentedFrames()));
                        report.insert(QString::fromUtf8("computeFrames"), static_cast<qint64>(second->presentedFrames()));
                        report.insert(QString::fromUtf8("sourceSwapchains"), static_cast<qint64>(first->swapchainGeneration()));
                        report.insert(QString::fromUtf8("computeSwapchains"), static_cast<qint64>(second->swapchainGeneration()));
                        report.insert(QString::fromUtf8("devicePixelRatio"), first->devicePixelRatio());
                        report.insert(QString::fromUtf8("viewportScenes"), 4);
                        passed = true;
                        timer.stop();
                        application.exit(0);
                    }

                    ++stage;
                } catch (const std::exception& error)
                {
                    std::cerr << "Vulkan Viewer test: " << error.what() << std::endl;
                    timer.stop();
                    application.exit(1);
                }
            });
            timer.start(1500);
            application.exec();
            // Containers destroy QWindows before the adopted instance is released.
        }
        require(passed, "Viewer smoke test did not complete.");
        unsigned headlessErrors = 0;

        for (auto &worker : headlessWorkers) {
            headlessErrors += worker.get();
        }

        require(headlessErrors == 0, "Concurrent headless contexts reported validation errors.");
        report.insert(QString::fromUtf8("headlessBeforePresentation"), true);
        report.insert(QString::fromUtf8("headlessWorkers"), 4);
        report.insert(QString::fromUtf8("headlessTransfers"), 16);
        report.insert(QString::fromUtf8("headlessValidationErrors"), static_cast<int>(headlessErrors));
        require(instance->native()->validationErrorCount() == 0, "Vulkan validation reported errors.");
        bool openGLLoaded = false;
#ifdef _WIN32
        openGLLoaded = GetModuleHandleW(L"opengl32.dll") != nullptr;
#endif
        QJsonArray loadStack;
        QString loadOrigin;

        for (const auto& module : moduleAudit.openGLLoadStack()) {
            loadStack.append(QString::fromUtf8(module.c_str()));

            if (loadOrigin.isEmpty()) {
                const auto name = QFileInfo(QString::fromUtf8(module.c_str())).fileName().toLower();

                if (name != QString::fromUtf8("ntdll.dll") && name != QString::fromUtf8("kernelbase.dll") &&
                    name != QString::fromUtf8("kernel32.dll")) {
                    loadOrigin = name;
                }
            }
        }

        // NVIDIA's Vulkan WSI loads opengl32 while creating a swapchain. Other
        // load origins must be investigated rather than silently accepted.
        require(!openGLLoaded || loadOrigin == QString::fromUtf8("nvoglv64.dll"), "Unexpected OpenGL DLL load origin.");
        report.insert(QString::fromUtf8("openGLLoadOrigin"), loadOrigin);
        report.insert(QString::fromUtf8("openGLLoadStack"), loadStack);
        report.insert(QString::fromUtf8("passed"), true);
        report.insert(QString::fromUtf8("validationErrors"), static_cast<int>(instance->native()->validationErrorCount()));
        report.insert(QString::fromUtf8("openGLLoaded"), openGLLoaded);
        QFile file(output + QString::fromUtf8("/report.json"));
        require(file.open(QIODevice::WriteOnly), "Could not write Viewer report.");
        const auto json = QJsonDocument(report).toJson();
        require(file.write(json) == json.size(), "Could not complete Viewer report.");
        std::cout << json.constData() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan Viewer test: " << error.what() << std::endl;
        return 1;
    }
}
