// SPDX-License-Identifier: GPL-2.0-or-later
// MinGW OCIO C++ API; Natron is accessed exclusively through OFX C ABI.
#include "OcioVulkan.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <mutex>
#include <sstream>
#include <vector>
#ifdef _WIN32
#define NATIVE_EXPORT extern "C" __declspec(dllexport)
#else
#define NATIVE_EXPORT extern "C"
#endif
namespace OCIO = OCIO_NAMESPACE;
namespace
{
const NatronOfxVulkanImageSuiteV1 *images = nullptr;
const NatronOfxVulkanShaderSuiteV2 *shaders = nullptr;
const OfxImageEffectSuiteV1 *effects = nullptr;
const OfxPropertySuiteV1 *properties = nullptr;
const NatronOfxVulkanRenderSuiteV1 *nativeRender = nullptr;
std::mutex reportMutex;
std::string report;
void require(bool value, const char *message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
void setHost(OfxHost *host)
{
    images = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    shaders = static_cast<const NatronOfxVulkanShaderSuiteV2 *>(host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 2));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    properties = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    nativeRender = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
}
OfxStatus action(const char *name, const void *handle, OfxPropertySetHandle, OfxPropertySetHandle)
{
    if (!images || !shaders || !effects || !properties) {
        return kOfxStatErrMissingHostFeature;
    }

    if (!std::strcmp(name, kOfxActionLoad) || !std::strcmp(name, kOfxActionUnload) ||
        !std::strcmp(name, kOfxActionCreateInstance) || !std::strcmp(name, kOfxActionDestroyInstance)) {
        return kOfxStatOK;
    }

    if (!std::strcmp(name, kOfxActionDescribe)) {
        OfxPropertySetHandle props = nullptr;
        effects->getPropertySet(static_cast<OfxImageEffectHandle>(const_cast<void *>(handle)), &props);
        properties->propSetString(props, kOfxPropLabel, 0, "OCIO Vulkan ABI test");
        properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGenerator);
        properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
        return kOfxStatOK;
    }

    if (!std::strcmp(name, kOfxImageEffectActionDescribeInContext)) {
        OfxPropertySetHandle props = nullptr;
        effects->clipDefine(static_cast<OfxImageEffectHandle>(const_cast<void *>(handle)), kOfxImageEffectOutputClipName, &props);
        properties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        return kOfxStatOK;
    }

    return kOfxStatReplyDefault;
}
float checkPixels(OcioVulkanProcessor &vulkan, const OCIO::ConstProcessorRcPtr &processor, unsigned channels, bool negative)
{
    const unsigned w = 41, h = 23, stride = w * channels + 7;
    std::vector<float> actual(stride * h, -987.f);
    float *first = actual.data() + (negative ? (h - 1) * stride : 0);
    const ptrdiff_t row = negative ? -ptrdiff_t(stride) : ptrdiff_t(stride);

    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (unsigned c = 0; c < channels; ++c) {
                first[y * row + x * channels + c] = c == 3 ? float((x + y) % 17) / 16.f : float((x * 7 + y * 3 + c * 11) % 89) / 37.f - .125f;
            }

    auto expected = actual;
    float *expectedFirst = expected.data() + (negative ? (h - 1) * stride : 0);
    OCIO::PackedImageDesc cpuImage(expectedFirst, w, h, channels, OCIO::BIT_DEPTH_F32,
                                   sizeof(float), channels * sizeof(float), row * ptrdiff_t(sizeof(float)));
    processor->getDefaultCPUProcessor()->apply(cpuImage);
    vulkan.apply(processor, first, w, h, channels, row * ptrdiff_t(sizeof(float)));
    float maximum = 0;

    for (size_t i = 0; i < actual.size(); ++i) {
        require(std::isfinite(actual[i]) && std::isfinite(expected[i]), "OCIO non-finite result.");
        float error = std::abs(actual[i] - expected[i]);
        maximum = std::max(maximum, error);
        require(error <= .00003f + std::abs(expected[i]) * .00003f, "OCIO CPU/GPU float result or row padding differs.");
    }

    return maximum;
}

// The algorithm receives an owned image through the same C view interface.
// Stock Reader integration separately checks real OFX clip loans and lifetimes.
struct ReaderOutput {
    NatronVulkanContextHandle context = 0;
    NatronVulkanImageHandle image = 0;
    OfxRectI bounds{}, window{};
    uint64_t uploads = 0, downloads = 0;
};
OfxStatus acquireReaderOutput(NatronVulkanContextHandle context, OfxPropertySetHandle propertiesHandle, NatronVulkanImageView* view)
{
    auto& output = *reinterpret_cast<ReaderOutput *>(propertiesHandle);
    output.context = context;
    const unsigned width = output.bounds.x2 - output.bounds.x1, height = output.bounds.y2 - output.bounds.y1;

    if (images->createImage(context, width, height, &output.image) != kOfxStatOK) {
        return kOfxStatErrMemory;
    }

    const std::vector<float> sentinel(size_t(width) * height * 4, -13.25f);

    if (images->upload(context, output.image, sentinel.data(), width * 16) != kOfxStatOK) {
        return kOfxStatFailed;
    }

    nativeRender->getTransferCounts(context, &output.uploads, &output.downloads);
    view->image = output.image;
    view->writable = 1;
    const int bounds[4] {output.bounds.x1, output.bounds.y1, output.bounds.x2, output.bounds.y2};
    const int window[4] {output.window.x1, output.window.y1, output.window.x2, output.window.y2};
    std::copy(bounds, bounds + 4, view->bounds);
    std::copy(window, window + 4, view->visibleBounds);
    return kOfxStatOK;
}
}
NATIVE_EXPORT const char *OfxReaderVulkanSelfTest()
{
    std::lock_guard<std::mutex> lock(reportMutex);

    try {
        require(images && shaders && nativeRender, "Reader native suites are unavailable.");
        OcioVulkanProcessor vulkan(images, shaders);
        auto transform = OCIO::CDLTransform::Create();
        const double slope[3] {.83, 1.07, .92}, offset[3] {.017, .025, .035}, power[3] {.85, 1.15, .9};
        transform->setSlope(slope);
        transform->setOffset(offset);
        transform->setPower(power);
        transform->setSat(.8);
        const auto processor = OCIO::Config::CreateRaw()->getProcessor(transform);
        const NatronOfxVulkanRenderSuiteV1 outputSuite{acquireReaderOutput, nullptr};
        float maximum = 0.f;
        unsigned cases = 0;
        uint64_t samples = 0;

        for (auto origin : {
                 std::array<int, 2> {0, 0}, {-11, -17}, {7, 13}
             }) {
            const OfxRectI bounds{origin[0], origin[1], origin[0] + 43, origin[1] + 29};
            const OfxRectI crop{bounds.x1 + 3, bounds.y1 + 5, bounds.x2 - 1, bounds.y2 - 1};

            for (bool negativeRows : {
                     false, true
                 }) {
                const int stride = 43 * 4 + 7, row = negativeRows ? -stride : stride;
                std::vector<float> storage(size_t(stride) * 29, 71.5f);
                float *first = storage.data() + (negativeRows ? 28 * stride : 0);

                for (int y = 0; y < 29; ++y) for (int x = 0; x < 43; ++x) {
                        float *pixel = first + y * row + x * 4;

                        for (unsigned c = 0; c < 3; ++c) {
                            pixel[c] = ((x * 3 + y * 7 + c * 13) % 79) / 38.f - .125f;
                        }

                        const float alpha[5] {-.125f, 0.f, 1e-8f, .37f, 1.f};
                        pixel[3] = alpha[(x + y) % 5];
                    }

                const auto untouched = storage;

                for (unsigned variant = 0; variant < 6; ++variant) for (unsigned level = 0; level < 6; ++level) {
                        const bool color = variant >= 2, unpremult = variant >= 4, premult = (variant & 1) != 0;
                        OfxRectI expectedBounds = crop;
                        std::vector<float> expected(size_t(crop.x2 - crop.x1) * (crop.y2 - crop.y1) * 4);
                        size_t index = 0;

                        for (int y = crop.y1; y < crop.y2; ++y) for (int x = crop.x1; x < crop.x2; ++x) {
                                const float *pixel = first + (y - bounds.y1) * row + (x - bounds.x1) * 4;
                                std::copy(pixel, pixel + 4, expected.data() + index);

                                if (unpremult && pixel[3] > 1.1920928955078125e-7f) {
                                    for (unsigned c = 0; c < 3; ++c) {
                                        expected[index + c] /= pixel[3];
                                    }
                                }

                                index += 4;
                            }

                        if (color) {
                            OCIO::PackedImageDesc data(expected.data(), crop.x2 - crop.x1, crop.y2 - crop.y1, 4);
                            processor->getDefaultCPUProcessor()->apply(data);
                        }

                        for (unsigned mip = 0; mip < level; ++mip) {
                            const OfxRectI nextBounds{int(std::floor(expectedBounds.x1 / 2.)), int(std::floor(expectedBounds.y1 / 2.)),
                                                      int(std::ceil(expectedBounds.x2 / 2.)), int(std::ceil(expectedBounds.y2 / 2.))};
                            std::vector<float> next(size_t(nextBounds.x2 - nextBounds.x1) * (nextBounds.y2 - nextBounds.y1) * 4);
                            size_t nextIndex = 0;

                            for (int y = nextBounds.y1; y < nextBounds.y2; ++y) for (int x = nextBounds.x1; x < nextBounds.x2; ++x) {
                                    unsigned count = 0;

                                    for (int sy = y * 2; sy <= y * 2 + 1; ++sy) for (int sx = x * 2; sx <= x * 2 + 1; ++sx) {
                                            if (sx < expectedBounds.x1 || sy < expectedBounds.y1 || sx >= expectedBounds.x2 || sy >= expectedBounds.y2) {
                                                continue;
                                            }

                                            const size_t source = (size_t(sy - expectedBounds.y1) * (expectedBounds.x2 - expectedBounds.x1) + sx - expectedBounds.x1) * 4;

                                            for (unsigned c = 0; c < 4; ++c) {
                                                next[nextIndex + c] += expected[source + c];
                                            }

                                            ++count;
                                        }

                                    for (unsigned c = 0; c < 4; ++c) {
                                        next[nextIndex + c] /= float(count);
                                    }

                                    nextIndex += 4;
                                }

                            expected = std::move(next);
                            expectedBounds = nextBounds;
                        }

                        if (premult) for (size_t i = 0; i < expected.size(); i += 4) {
                                const float alpha = expected[i + 3] > 0.f ? expected[i + 3] : 0.f;

                                for (unsigned c = 0; c < 3; ++c) {
                                    expected[i + c] *= alpha;
                                }

                                expected[i + 3] = alpha;
                            }

                        ReaderOutput destination;
                        destination.window = expectedBounds;
                        destination.bounds = {expectedBounds.x1 - 2, expectedBounds.y1 - 3, expectedBounds.x2 + 2, expectedBounds.y2 + 3};

                        try {
                            vulkan.uploadReader(color ? processor : OCIO::ConstProcessorRcPtr(), outputSuite, first, bounds,
                                                int64_t(row) * sizeof(float), crop, reinterpret_cast<OfxPropertySetHandle>(&destination),
                                                destination.window, level, unpremult, premult);
                            uint64_t uploads = 0, downloads = 0;
                            nativeRender->getTransferCounts(destination.context, &uploads, &downloads);
                            require(uploads - destination.uploads == 1 && downloads == destination.downloads, "Reader mipmap staged color pixels through CPU.");
                            const unsigned width = destination.bounds.x2 - destination.bounds.x1, height = destination.bounds.y2 - destination.bounds.y1;
                            std::vector<float> actual(size_t(width) * height * 4);
                            require(images->download(destination.context, destination.image, actual.data(), width * 16) == kOfxStatOK, "Reader output download failed.");

                            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) for (unsigned c = 0; c < 4; ++c) {
                                        const int gx = destination.bounds.x1 + x, gy = destination.bounds.y1 + y;
                                        const bool inside = gx >= expectedBounds.x1 && gy >= expectedBounds.y1 && gx < expectedBounds.x2 && gy < expectedBounds.y2;
                                        const float reference = inside ? expected[(size_t(gy - expectedBounds.y1) * (expectedBounds.x2 - expectedBounds.x1) + gx - expectedBounds.x1) * 4 + c] : -13.25f;
                                        const float value = actual[(size_t(y) * width + x) * 4 + c], error = std::abs(value - reference);
                                        require(std::isfinite(value) && error <= (inside ? .00003f + .00003f * std::abs(reference) : 0.f), "Reader CPU/GPU pixels or ROI guards differ.");
                                        maximum = std::max(maximum, error);
                                        ++samples;
                                    }
                        } catch (...) {
                            if (destination.image) {
                                images->releaseImage(destination.context, destination.image);
                            }

                            throw;
                        }

                        images->releaseImage(destination.context, destination.image);
                        require(storage == untouched, "Reader modified the decoder buffer or padding.");
                        ++cases;
                    }
            }
        }
        const auto info = vulkan.deviceInfo();
        require(info.validationEnabled && info.validationErrors == 0, "Reader Vulkan validation failed.");
        std::ostringstream json;
        json << "{\"passed\":true,\"cases\":" << cases << ",\"samples\":" << samples << ",\"maxAbsoluteError\":" << maximum
             << ",\"uploadsPerCase\":1,\"processingDownloads\":0,\"signedPaddedRows\":true,\"roiGuards\":true,\"validationErrors\":0}";
        report = json.str();
    } catch (const std::exception& error) {
        report = std::string("Reader Vulkan test failure: ") + error.what();
    }

    return report.c_str();
}
namespace
{
struct WriterLoan {
    NatronVulkanContextHandle context = 0;
    NatronVulkanImageHandle image = 0;
    OfxRectI bounds{}, window{};
    std::vector<float> seed;
    uint64_t uploads = 0, downloads = 0;
    bool writable = false;
    ~WriterLoan()
    {
        if (image) {
            images->releaseImage(context, image);
        }
    }
};
OfxStatus acquireWriterLoan(NatronVulkanContextHandle context, OfxPropertySetHandle handle, NatronVulkanImageView* view)
{
    auto& loan = *reinterpret_cast<WriterLoan *>(handle);
    const unsigned width = loan.bounds.x2 - loan.bounds.x1, height = loan.bounds.y2 - loan.bounds.y1;

    if (!loan.image) {
        loan.context = context;

        if (images->createImage(context, width, height, &loan.image) != kOfxStatOK ||
            images->upload(context, loan.image, loan.seed.data(), width * 16) != kOfxStatOK) {
            return kOfxStatFailed;
        }

        nativeRender->getTransferCounts(context, &loan.uploads, &loan.downloads);
    }

    if (context != loan.context) {
        return kOfxStatFailed;
    }

    view->image = loan.image;
    view->writable = loan.writable;
    const int bounds[4] {loan.bounds.x1, loan.bounds.y1, loan.bounds.x2, loan.bounds.y2};
    const int window[4] {loan.window.x1, loan.window.y1, loan.window.x2, loan.window.y2};
    std::copy(bounds, bounds + 4, view->bounds);
    std::copy(window, window + 4, view->visibleBounds);
    return kOfxStatOK;
}
}
NATIVE_EXPORT const char *OfxWriterVulkanSelfTest()
{
    std::lock_guard<std::mutex> lock(reportMutex);

    try {
        require(images && shaders && nativeRender, "Writer native suites unavailable.");
        OcioVulkanProcessor vulkan(images, shaders);
        auto transform = OCIO::CDLTransform::Create();
        const double slope[3] {.83, 1.07, .92}, offset[3] {.017, .025, .035}, power[3] {.85, 1.15, .9};
        transform->setSlope(slope);
        transform->setOffset(offset);
        transform->setPower(power);
        transform->setSat(.8);
        const auto processor = OCIO::Config::CreateRaw()->getProcessor(transform);
        const NatronOfxVulkanRenderSuiteV1 suite{acquireWriterLoan, nullptr};
        unsigned cases = 0;
        uint64_t samples = 0;
        float maximum = 0.f;
        // opaque/unpremult/premult variants include both processing orders.
        const bool modes[6][3] {{false, false, false}, {false, true, false}, {false, false, true},
            {false, true, true}, {true, false, false}, {true, false, true}
        };

        for (float alpha : {
                 0.f, 1e-8f, -.125f, .37f, 1.f
             })

            for (bool color : {
                     false, true
                 })

                for (bool negativeRows : {
                         false, true
                     })

                    for (bool cropped : {
                             false, true
                         })

                        for (const auto& mode : modes) {
                            WriterLoan source, destination;
                            source.bounds = cropped ? OfxRectI{-9, 13, 2, 20} :
                                            OfxRectI{0, 0, 11, 7};
                            source.window = source.bounds;
                            const OfxRectI window = cropped ? OfxRectI{-7, 14, 0, 19} :
                                                    source.bounds;
                            const unsigned width = window.x2 - window.x1, height = window.y2 - window.y1;
                            source.seed.resize(11 * 7 * 4);

                            for (size_t i = 0; i < source.seed.size(); ++i) {
                                source.seed[i] = i % 4 == 3 ? alpha : float((i * 17) % 79) / 38.f-.125f;
                            }

                            std::vector<float> expected(size_t(width)*height * 4);

                            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                                    const size_t from = ((window.y1 - source.bounds.y1 + y) * 11 + window.x1 - source.bounds.x1 + x) * 4;
                                    float *pixel = &expected[(size_t(y) * width + x) * 4];
                                    std::copy_n(&source.seed[from], 4, pixel);

                                    if (mode[0]) {
                                        pixel[3] = 1.f;
                                    }

                                    if (mode[1] && pixel[3] > 1.1920928955078125e-7f)
                                        for (unsigned c = 0; c < 3; ++c) {
                                            pixel[c] /= pixel[3];
                                        }
                                }

                            if (color) {
                                OCIO::PackedImageDesc pixels(expected.data(), width, height, 4);
                                processor->getDefaultCPUProcessor()->apply(pixels);
                            }

                            if (mode[2]) for (size_t i = 0; i < expected.size(); i += 4) {
                                    const float a = expected[i + 3] > 0.f ? expected[i + 3] : 0.f;

                                    for (unsigned c = 0; c < 3; ++c) {
                                        expected[i + c] *= a;
                                    }

                                    expected[i + 3] = a;
                                }

                            const size_t row = width * 4 + 7, guard = 11;
                            std::vector<float> actual(row * height + guard * 2, -13.25f), padded = actual;
                            float *first = actual.data() + guard + (negativeRows ? (height - 1) * row : 0);

                            for (unsigned y = 0; y < height; ++y)
                                std::copy_n(expected.data() + size_t(y) * width * 4, width * 4,
                                            padded.data() + guard + (negativeRows ? height - 1 - y : y) * row);

                            vulkan.downloadWriter(color ? processor : OCIO::ConstProcessorRcPtr(), suite,
                                                  reinterpret_cast<OfxPropertySetHandle>(&source), window, first,
                                                  int64_t(row)*sizeof(float) * (negativeRows ? -1 : 1), mode[0], mode[1], mode[2]);
                            uint64_t uploads = 0, downloads = 0;
                            nativeRender->getTransferCounts(source.context, &uploads, &downloads);
                            require(uploads == source.uploads && downloads == source.downloads + 1, "Writer encoder boundary must download once and never upload.");

                            for (size_t i = 0; i < actual.size(); ++i) {
                                const float error = std::abs(actual[i] - padded[i]);
                                require(std::isfinite(actual[i]) && error <= 3e-5f+3e-5f * std::abs(padded[i]), "Writer color/alpha/signed-row reference differs.");

                                if (padded[i] == -13.25f) {
                                    require(actual[i] == padded[i], "Writer modified row padding/guards.");
                                }

                                maximum = std::max(maximum, error);
                                ++samples;
                            }

                            destination.bounds = {window.x1 - 2, window.y1 - 1, window.x2 + 2, window.y2 + 1};
                            destination.window = window;
                            destination.writable = true;
                            const unsigned dw = destination.bounds.x2 - destination.bounds.x1, dh = destination.bounds.y2 - destination.bounds.y1;
                            destination.seed.assign(size_t(dw)*dh * 4, -13.25f);
                            vulkan.copyClip(suite, reinterpret_cast<OfxPropertySetHandle>(&source),
                                            reinterpret_cast<OfxPropertySetHandle>(&destination), window);
                            nativeRender->getTransferCounts(source.context, &uploads, &downloads);
                            require(uploads == destination.uploads && downloads == destination.downloads, "Writer pass-through staged pixels through CPU.");
                            std::vector<float> original(source.seed.size()), copied(destination.seed.size());
                            require(images->download(source.context, source.image, original.data(), 11 * 16) == kOfxStatOK && original == source.seed,
                                    "Writer modified immutable upstream image.");
                            require(images->download(destination.context, destination.image, copied.data(), dw * 16) == kOfxStatOK, "Writer pass-through readback failed.");

                            for (int y = destination.bounds.y1; y < destination.bounds.y2; ++y)
                                for (int x = destination.bounds.x1; x < destination.bounds.x2; ++x)
                                    for (unsigned c = 0; c < 4; ++c) {
                                        const float expectedValue = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2 ?
                                                                    source.seed[((y - source.bounds.y1) * 11 + x - source.bounds.x1) * 4 + c] : -13.25f;
                                        require(copied[((y - destination.bounds.y1)*dw + x - destination.bounds.x1) * 4 + c] == expectedValue,
                                                "Writer pass-through/ROI guard differs from original source.");
                                    }

                            ++cases;
                        }

        const auto info = vulkan.deviceInfo();
        require(info.validationEnabled && !info.validationErrors, "Writer Vulkan validation failed.");
        std::ostringstream json;
        json << "{\"passed\":true,\"cases\":" << cases << ",\"samples\":" << samples << ",\"maxAbsoluteError\":" << maximum
             << ",\"processingUploads\":0,\"encoderDownloads\":1,\"sourceImmutable\":true,\"passThroughOriginal\":true,\"signedPaddedRows\":true,\"roiGuards\":true,\"validationErrors\":0}";
        report = json.str();
    } catch (const std::exception& error) {
        report = std::string("Writer Vulkan test failure: ") + error.what();
    }

    return report.c_str();
}
NATIVE_EXPORT const char *OfxOCIOVulkanSelfTest()
{
    std::lock_guard<std::mutex> lock(reportMutex);

    try {
        OcioVulkanProcessor vulkan(images, shaders);
        auto config = OCIO::Config::CreateRaw();
        std::vector<OCIO::ConstProcessorRcPtr> processors;
        auto add = [&](const OCIO::ConstTransformRcPtr & transform) {
            processors.push_back(config->getProcessor(transform));
        };
        auto matrix = OCIO::MatrixTransform::Create();
        double values[16] {1.125, 0, 0, 0, 0, .75, 0, 0, 0, 0, 1.25, 0, 0, 0, 0, .625};
        double offsets[4] {.1, -.02, .03, .2};
        matrix->setMatrix(values);
        matrix->setOffset(offsets);
        add(matrix);
        auto exponent = OCIO::ExponentTransform::Create();
        double exponents[4] {.75, 1.1, 1.25, 1};
        exponent->setValue(exponents);
        add(exponent);
        auto cdl = OCIO::CDLTransform::Create();
        double slopes[3] {1.1, .85, 1.25}, powers[3] {1.1, .9, 1.2};
        cdl->setSlope(slopes);
        cdl->setPower(powers);
        cdl->setSat(.8);
        add(cdl);
        auto log = OCIO::LogTransform::Create();
        log->setBase(2);
        add(log);
        log->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
        add(log);

        for (auto interpolation : {
                 OCIO::INTERP_NEAREST, OCIO::INTERP_LINEAR
             }) {
            auto lut = OCIO::Lut1DTransform::Create(1024, false);
            lut->setInterpolation(interpolation);

            for (unsigned i = 0; i < 1024; ++i) {
                float v = i / 1023.f;
                lut->setValue(i, std::pow(v, 1.2f), std::pow(v, .8f), std::pow(v, 1.1f));
            }

            add(lut);
        }
        auto lut3 = OCIO::Lut3DTransform::Create(17);
        lut3->setInterpolation(OCIO::INTERP_TETRAHEDRAL);

        for (unsigned b = 0; b < 17; ++b) for (unsigned g = 0; g < 17; ++g) for (unsigned r = 0; r < 17; ++r) {
                    lut3->setValue(r, g, b, std::pow(r / 16.f, 1.2f), std::pow(g / 16.f, .8f), std::pow(b / 16.f, 1.1f));
                }

        add(lut3);

        for (auto interpolation : {
                 OCIO::INTERP_NEAREST, OCIO::INTERP_LINEAR
             }) {
            auto wide = OCIO::Lut1DTransform::Create(32768, false);
            wide->setInterpolation(interpolation);

            for (unsigned i = 0; i < 32768; ++i) {
                float v = i / 32767.f;
                wide->setValue(i, std::pow(v, 1.2f), std::pow(v, .8f), std::pow(v, 1.1f));
            }

            add(wide);
        }
        auto group = OCIO::GroupTransform::Create();

        for (unsigned i = 0; i < 7; ++i) {
            group->appendTransform(lut3);
        }

        add(group);
        auto exposure = OCIO::ExposureContrastTransform::Create();
        exposure->makeExposureDynamic();
        exposure->setExposure(.5);
        add(exposure);
        exposure->setExposure(1.25);
        add(exposure);
        auto primary = OCIO::GradingPrimaryTransform::Create(OCIO::GRADING_LIN);
        OCIO::GradingPrimary grade(OCIO::GRADING_LIN);
        grade.m_offset = OCIO::GradingRGBM(.01, .02, .03, 0);
        grade.m_exposure = OCIO::GradingRGBM(.1, -.1, .2, .15);
        primary->setValue(grade);
        primary->makeDynamic();
        add(primary);
        auto curves = OCIO::GradingRGBCurveTransform::Create(OCIO::GRADING_LIN);
        auto curveValues = OCIO::GradingRGBCurve::Create(OCIO::GRADING_LIN);
        auto red = curveValues->getCurve(OCIO::RGB_RED);
        red->setNumControlPoints(3);
        red->getControlPoint(0) = OCIO::GradingControlPoint(0.f, 0.f);
        red->getControlPoint(1) = OCIO::GradingControlPoint(.5f, .4f);
        red->getControlPoint(2) = OCIO::GradingControlPoint(1.f, .95f);
        curves->setValue(curveValues);
        curves->makeDynamic();
        add(curves);
        float maximum = 0;
        unsigned cases = 0;
        std::array<uint32_t, 5> resources{};

        for (const auto &processor : processors) for (unsigned channels : {
                        3u, 4u

                    }) for (bool negative : {
                                    false, true
                                }) {
                    maximum = std::max(maximum, checkPixels(vulkan, processor, channels, negative));
                    auto info = vulkan.resourceInfo();

                    for (unsigned i = 0; i < 4; ++i) {
                        resources[i] = std::max(resources[i], info[i]);
                    }

                    resources[4] |= info[4];
                    ++cases;
                }
        std::vector<std::future<float>> workers;

        for (unsigned i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, [&] { return checkPixels(vulkan, processors[0], 4, true); }));

        for (auto &worker : workers) {
            maximum = std::max(maximum, worker.get());
            ++cases;
        }

        auto info = vulkan.deviceInfo();
        require(resources[0] && resources[1] && resources[2] >= 7 && resources[4] == 31, "OCIO resource coverage is incomplete.");
        require(info.validationEnabled && info.validationErrors == 0, "OCIO Vulkan validation failed.");
        std::ostringstream json;
        json << "{\"passed\":true,\"referenceCases\":" << cases << ",\"processors\":" << processors.size()
             << ",\"hostWorkers\":4,\"maxAbsoluteError\":" << maximum << ",\"validationErrors\":0,\"signedPaddedRows\":true"
             << ",\"textures1D\":" << resources[0] << ",\"textures2D\":" << resources[1] << ",\"textures3D\":" << resources[2]
             << ",\"uniformTypeMask\":" << resources[4] << ",\"device\":\"" << info.name << "\"}";
        report = json.str();
    } catch (const std::exception &error) {
        report = std::string("OCIO Vulkan test failure: ") + error.what();
    }

    return report.c_str();
}
NATIVE_EXPORT int OfxGetNumberOfPlugins()
{
    return 1;
}
NATIVE_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    static OfxPlugin plugin{kOfxImageEffectPluginApi, 1, "org.natron.test.OcioVulkanSuite", 1, 0, setHost, action};
    return index == 0 ? &plugin : nullptr;
}
