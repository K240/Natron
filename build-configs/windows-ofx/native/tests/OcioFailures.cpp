// SPDX-License-Identifier: GPL-2.0-or-later
#include "OcioVulkan.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;
namespace
{
enum class Stage { None, DeviceInfo, Program, Image, Upload, Render, Download, Release };
struct Host {
    Stage failure = Stage::None;
    OfxStatus status = kOfxStatFailed;
    bool reached = false, context = false, program = false, image = false;
    uint32_t width = 0, height = 0;
    std::vector<float> pixels;
    OCIO::ConstCPUProcessorRcPtr cpu;
} host;

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
OfxStatus failure(Stage stage)
{
    if (host.failure == stage && !host.reached) {
        host.reached = true;
        return host.status;
    }

    return kOfxStatOK;
}
OfxStatus createContext(int, int, NatronVulkanContextHandle *context)
{
    require(!host.context, "Context leaked between tests");
    host.context = true;
    *context = 1;
    return kOfxStatOK;
}
OfxStatus releaseContext(NatronVulkanContextHandle)
{
    host.context = host.program = host.image = false;
    host.pixels.clear();
    return kOfxStatOK;
}
OfxStatus deviceInfo(NatronVulkanContextHandle, NatronVulkanDeviceInfo *info)
{
    if (auto status = failure(Stage::DeviceInfo); status != kOfxStatOK) {
        return status;
    }

    std::strcpy(info->name, "Fault injection host (no GPU)");
    return kOfxStatOK;
}
OfxStatus createImage(NatronVulkanContextHandle, uint32_t width, uint32_t height, NatronVulkanImageHandle *image)
{
    *image = 0;

    if (auto status = failure(Stage::Image); status != kOfxStatOK) {
        return status;
    }

    require(!host.image, "Temporary image leaked before retry");
    host.image = true;
    host.width = width;
    host.height = height;
    host.pixels.assign(size_t(width) * height * 4, 0.f);
    *image = 2;
    return kOfxStatOK;
}
OfxStatus releaseImage(NatronVulkanContextHandle, NatronVulkanImageHandle)
{
    if (auto status = failure(Stage::Release); status != kOfxStatOK) {
        return status;
    }

    require(host.image, "Temporary image released twice");
    host.image = false;
    return kOfxStatOK;
}
OfxStatus upload(NatronVulkanContextHandle, NatronVulkanImageHandle, const void *pixels, int64_t stride)
{
    if (auto status = failure(Stage::Upload); status != kOfxStatOK) {
        return status;
    }

    for (uint32_t y = 0; y < host.height; ++y) {
        std::memcpy(host.pixels.data() + size_t(y) * host.width * 4,
                    static_cast<const char *>(pixels) + int64_t(y) * stride, size_t(host.width) * 4 * sizeof(float));
    }

    return kOfxStatOK;
}
OfxStatus download(NatronVulkanContextHandle, NatronVulkanImageHandle, void *pixels, int64_t stride)
{
    for (uint32_t y = 0; y < host.height; ++y) {
        std::memcpy(static_cast<char *>(pixels) + int64_t(y) * stride,
                    host.pixels.data() + size_t(y) * host.width * 4, size_t(host.width) * 4 * sizeof(float));

        // A failed readback can already have modified its private destination.
        if (auto status = failure(Stage::Download); status != kOfxStatOK) {
            return status;
        }
    }

    return kOfxStatOK;
}
OfxStatus createProgram(NatronVulkanContextHandle, const char *, uint32_t,
                        const NatronVulkanShaderLayout *, NatronVulkanProgramHandle *program, char *error, uint32_t capacity)
{
    *program = 0;

    if (capacity) {
        error[0] = '\0';
    }

    if (auto status = failure(Stage::Program); status != kOfxStatOK) {
        return status;
    }

    require(!host.program, "Program leaked before retry");
    host.program = true;
    *program = 3;
    return kOfxStatOK;
}
OfxStatus releaseProgram(NatronVulkanContextHandle, NatronVulkanProgramHandle)
{
    host.program = false;
    return kOfxStatOK;
}
OfxStatus render(NatronVulkanContextHandle, NatronVulkanProgramHandle, NatronVulkanImageHandle,
                 const NatronVulkanRegion *, const NatronVulkanShaderInput[4], const void *, uint32_t)
{
    // Emulate successful GPU pixels with an OCIO CPU reference. This test checks
    // the callback's transaction and error boundary, not shader correctness.
    for (size_t i = 0; i < host.pixels.size(); i += 4) {
        host.cpu->applyRGBA(host.pixels.data() + i);
    }

    return failure(Stage::Render);
}
float *firstRow(std::vector<float> &pixels, unsigned stride, bool reversed)
{
    return pixels.data() + (reversed ? 2 * stride : 0);
}
void cpuBuffer(std::vector<float> &pixels, unsigned components, unsigned stride, bool reversed)
{
    float *first = firstRow(pixels, stride, reversed);

    for (int y = 0; y < 3; ++y) {
        auto *row = first + (reversed ? -y : y) * int(stride);

        for (int x = 0; x < 5; ++x) {
            float rgba[4] {0, 0, 0, 1};
            std::copy_n(row + x * components, components, rgba);
            host.cpu->applyRGBA(rgba);
            std::copy_n(rgba, components, row + x * components);
        }
    }
}
}
int main()
{
    try {
        NatronOfxVulkanImageSuiteV1 images{};
        images.createContext = createContext;
        images.releaseContext = releaseContext;
        images.getDeviceInfo = deviceInfo;
        images.createImage = createImage;
        images.releaseImage = releaseImage;
        images.upload = upload;
        images.download = download;
        NatronOfxVulkanShaderSuiteV2 shaders{};
        shaders.createProgram = createProgram;
        shaders.releaseProgram = releaseProgram;
        shaders.render = render;
        auto transform = OCIO::MatrixTransform::Create();
        const double matrix[16] {1.1, 0, 0, 0, 0, .8, 0, 0, 0, 0, 1.3, 0, 0, 0, 0, 1};
        const double offset[4] {.03, -.02, .07, 0};
        transform->setMatrix(matrix);
        transform->setOffset(offset);
        auto processor = OCIO::Config::CreateRaw()->getProcessor(transform);
        host.cpu = processor->getDefaultCPUProcessor();
        unsigned cases = 0, retries = 0;

        for (unsigned components : {
                 3u, 4u
             }) {
            for (bool reversed : {
                     false, true
                 }) {
                const unsigned stride = 5 * components + 7; // Padding is part of the transaction.
                const int64_t rowBytes = int64_t(stride * sizeof(float)) * (reversed ? -1 : 1);
                std::vector<float> original(size_t(stride) * 3, -937.f);

                for (unsigned y = 0; y < 3; ++y) {
                    for (unsigned x = 0; x < 5; ++x) {
                        for (unsigned c = 0; c < components; ++c) {
                            original[y * stride + x * components + c] = float(x * 3 + y + c) / 17;
                        }
                    }
                }

                auto expected = original;
                cpuBuffer(expected, components, stride, reversed);

                for (Stage stage : {
                         Stage::None, Stage::DeviceInfo, Stage::Program, Stage::Image,
                         Stage::Upload, Stage::Render, Stage::Download, Stage::Release
                     }) {
                    for (OfxStatus status : {
                             kOfxStatFailed, kOfxStatErrMemory, kOfxStatErrValue
                         }) {
                        if (stage == Stage::None && status != kOfxStatFailed) {
                            continue;
                        }

                        host.failure = stage;
                        host.status = status;
                        host.reached = false;
                        auto pixels = original;
                        NatronVulkanDeviceInfo info{};
                        std::strcpy(info.name, "unchanged");
                        {
                            OcioVulkanProcessor state(&images, &shaders);
                            const bool success = state.tryApply(processor, firstRow(pixels, stride, reversed),
                                                                5, 3, components, rowBytes, &info);
                            require(success == (stage == Stage::None), "Wrong optional acceleration result");
                            require(!host.image, "Failed operation leaked its temporary image");

                            if (!success) {
                                require(host.reached, "Fault injection did not reach the requested operation");
                                require(pixels == original, "Failure modified caller pixels or row padding");
                                require(std::strcmp(info.name, "unchanged") == 0, "Failure committed device metadata");
                                auto fallback = pixels;
                                cpuBuffer(fallback, components, stride, reversed);
                                require(fallback == expected, "CPU fallback applied a double transform");
                                host.failure = Stage::None;
                                require(state.tryApply(processor, firstRow(pixels, stride, reversed),
                                                       5, 3, components, rowBytes), "Retry after transient failure failed");
                                ++retries;
                            }

                            require(pixels == expected, "Successful acceleration changed padding or reference pixels");
                            require(!host.image, "Successful operation leaked its temporary image");
                        }
                        require(!host.context && !host.program && !host.image, "Processor teardown leaked resources");
                        ++cases;
                    }
                }
            }
        }
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"transient_retries\":" << retries
                  << ",\"gpu_required\":false,\"scope\":\"OCIO CPU callback failure transaction\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
