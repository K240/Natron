// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/Image.h"
#include "Engine/VulkanImageStorage.h"
#include "Gpu/VulkanDevice.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace Natron;
namespace
{
const RectI bounds(-11, -7, 26, 16), small(-9, -5, 20, 14), roi(-6, -3, 18, 13);
const ViewerColorSpaceEnum spaces[] = {eViewerColorSpaceLinear, eViewerColorSpaceSRGB, eViewerColorSpaceRec709, eViewerColorSpaceBT1886};
const ImageBitDepthEnum depths[] = {eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthFloat};
void require(bool value, const char* error)
{
    if (!value) {
        throw std::runtime_error(error);
    }
}
const ImagePlaneDesc &plane(unsigned components)
{
    switch (components) {
    case 1:
        return ImagePlaneDesc::getAlphaComponents();

    case 2:
        return ImagePlaneDesc::getXYComponents();

    case 3:
        return ImagePlaneDesc::getRGBComponents();

    default:
        return ImagePlaneDesc::getRGBAComponents();
    }
}
ImagePtr ram(const RectI& box, unsigned components = 4, ImageBitDepthEnum depth = eImageBitDepthFloat)
{
    return std::make_shared<Image>(plane(components), RectD(box.x1, box.y1, box.x2, box.y2), box, 2, 1.25, depth,
                                   eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
}
ImagePtr gpu(const std::shared_ptr<VulkanImageContext> &context, const RectI& box)
{
    // Match the cache constructor used by Engine: it tracks rendered pixels.
    // createVulkan is a temporary image and deliberately has no bitmap.
    auto params = Image::makeVulkanParams(context, RectD(box.x1, box.y1, box.x2, box.y2), box, 1.25, 2);
    auto image = std::make_shared<Image>(ImageKey(), params, nullptr);
    image->allocateMemory();
    return image;
}
void pattern(Image& image, unsigned seed)
{
    const auto box = image.getBounds();
    const unsigned channels = image.getComponents().getNumComponents();
    auto pixels = image.getWriteRights();

    for (int y = box.y1; y < box.y2; ++y) for (int x = box.x1; x < box.x2; ++x) {
            auto* pixel = pixels.pixelAt(x, y);

            for (unsigned c = 0; c < channels; ++c) {
                const unsigned value = ((x - box.x1) * 17 + (y - box.y1) * 23 + c * 31 + seed * 13) % 177;

                if (image.getBitDepth() == eImageBitDepthByte) {
                    reinterpret_cast<unsigned char *>(pixel)[c] = value;
                } else if (image.getBitDepth() == eImageBitDepthShort) {
                    reinterpret_cast<unsigned short *>(pixel)[c] = value * 359;
                } else {
                    reinterpret_cast<float *>(pixel)[c] = c == 3 ? float(value % 5) / 4.f : float(value) / 80.f - .3f;
                }
            }
        }
}
void initialBitmap(Image& image)
{
    auto box = image.getBounds();
    box.x1 = std::max(box.x1, 12);
    image.markForRendered(box);
}
ImagePtr readNative(const Image& native)
{
    auto result = ram(native.getBounds());
    result->pasteFrom(native, native.getBounds(), true);
    return result;
}
float compare(const Image& actual, const Image& expected)
{
    require(actual.getBounds() == expected.getBounds() && actual.getComponents() == expected.getComponents() &&
            actual.getBitDepth() == expected.getBitDepth() && actual.getRoD() == expected.getRoD() &&
            actual.getMipmapLevel() == expected.getMipmapLevel() && actual.getPixelAspectRatio() == expected.getPixelAspectRatio() &&
            actual.getPremultiplication() == expected.getPremultiplication() && actual.getFieldingOrder() == expected.getFieldingOrder(),
            "Conversion changed output metadata.");
    const auto box = expected.getBounds();
    const unsigned channels = expected.getComponents().getNumComponents();
    auto a = actual.getReadRights(), e = expected.getReadRights();
    float maximum = 0;

    for (int y = box.y1; y < box.y2; ++y) for (int x = box.x1; x < box.x2; ++x) for (unsigned c = 0; c < channels; ++c) {
                const auto* ap = a.pixelAt(x, y);
                const auto* ep = e.pixelAt(x, y);

                if (expected.getBitDepth() == eImageBitDepthByte) {
                    require(reinterpret_cast<const unsigned char*>(ap)[c] == reinterpret_cast<const unsigned char*>(ep)[c], "Byte conversion differs from CPU reference.");
                } else if (expected.getBitDepth() == eImageBitDepthShort) {
                    require(reinterpret_cast<const unsigned short*>(ap)[c] == reinterpret_cast<const unsigned short*>(ep)[c], "16-bit conversion differs from CPU reference.");
                } else {
                    const float av = reinterpret_cast<const float *>(ap)[c], ev = reinterpret_cast<const float*>(ep)[c];

                    if (std::isnan(ev)) {
                        require(std::isnan(av), "Conversion changed NaN behavior.");
                    } else if (std::isinf(ev)) {
                        require(av == ev, "Conversion changed infinity behavior.");
                    } else {
                        const float delta = std::abs(av - ev);
                        require(std::isfinite(av) && delta <= 3e-5f + std::abs(ev) * 3e-5f, "Float conversion differs from CPU reference.");
                        maximum = std::max(maximum, delta);
                    }
                }
            }

    std::list<RectI> aRest, eRest;
    actual.getRestToRender(box, aRest);
    expected.getRestToRender(box, eRest);
    require(aRest == eRest, "Conversion changed bitmap validity/outside ROI.");
    return maximum;
}
void convert(const Image& source, Image& output, ViewerColorSpaceEnum from, ViewerColorSpaceEnum to, int alpha, bool alpha0,
             bool bitmap, bool unpremult)
{
    // The established CPU byte converter uses randomized error diffusion.
    // Keep its random sequence identical to the independent reference call.
    std::srand(79);

    if (alpha0) {
        source.convertToFormatAlpha0(roi, from, to, alpha, bitmap, unpremult, &output);
    } else {
        source.convertToFormat(roi, from, to, alpha, bitmap, unpremult, &output);
    }
}
}
int main(int argc, char** argv)
{
    try {
        int index = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            index = std::stoi(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: NatronVulkanImageConvert [--gpu index]");
        }

        QCoreApplication application(argc, argv);
        Gpu::InstanceOptions options;
        options.validation = true;
        auto instance = Gpu::VulkanInstance::create(options);
        auto device = Gpu::VulkanDevice::create(instance, index);
        auto context = std::make_shared<VulkanImageContext>(device);
        unsigned uploads = 0, downloads = 0, nativeCases = 0, crossContexts = 0;
        float maximum = 0;

        for (auto depth : depths) for (unsigned components = 1; components <= 4; ++components) {
                auto input = ram(bounds, components, depth);
                pattern(*input, components + unsigned(depth));
                input->markForRendered(RectI(bounds.x1, bounds.y1, 0, bounds.y2));

                for (auto from : spaces) for (auto to : spaces) for (unsigned alpha0 = 0; alpha0 < 2; ++alpha0) {
                            auto expected = ram(small), initial = ram(small);
                            pattern(*expected, 41);
                            pattern(*initial, 41);
                            initialBitmap(*expected);
                            initialBitmap(*initial);
                            auto actual = gpu(context, small);
                            actual->pasteFrom(*initial, small, true);
                            const bool bitmap = uploads % 2 == 0;
                            convert(*input, *expected, from, to, components - 1, alpha0, bitmap, true);
                            convert(*input, *actual, from, to, components - 1, alpha0, bitmap, true);
                            maximum = std::max(maximum, compare(*readNative(*actual), *expected));
                            ++uploads;
                        }
            }

        auto input = ram(bounds);
        pattern(*input, 23);
        input->markForRendered(RectI(bounds.x1, bounds.y1, 0, bounds.y2));
        auto native = gpu(context, bounds);
        native->pasteFrom(*input, bounds, true);

        for (auto depth : depths) for (unsigned components = 1; components <= 4; ++components)
                for (auto from : spaces) for (auto to : spaces) for (unsigned alpha0 = 0; alpha0 < 2; ++alpha0)
                            for (unsigned unpremult = 0; unpremult < 2; ++unpremult) {
                                auto expected = ram(small, components, depth), actual = ram(small, components, depth);
                                pattern(*expected, 37);
                                pattern(*actual, 37);
                                initialBitmap(*expected);
                                initialBitmap(*actual);
                                const bool bitmap = downloads % 2 == 0;
                                const int alpha = downloads % 5 == 4 ? -1 : int(downloads % 4);
                                convert(*input, *expected, from, to, alpha, alpha0, bitmap, unpremult);
                                convert(*native, *actual, from, to, alpha, alpha0, bitmap, unpremult);
                                maximum = std::max(maximum, compare(*actual, *expected));
                                ++downloads;
                            }

        for (auto from : spaces) for (auto to : spaces) for (unsigned alias = 0; alias < 2; ++alias)
                    for (unsigned shifted = 0; shifted < 2; ++shifted) {
                        const RectI box = alias || !shifted ? bounds : small;
                        auto initial = ram(box), expected = ram(box);

                        if (alias) {
                            initial->pasteFrom(*input, box, true);
                            expected->pasteFrom(*input, box, true);
                        } else {
                            pattern(*initial, 29);
                            pattern(*expected, 29);
                            initialBitmap(*initial);
                            initialBitmap(*expected);
                        }

                        auto actual = gpu(context, box);
                        actual->pasteFrom(*initial, box, true);
                        convert(*input, *expected, from, to, 3, true, true, true);
                        const auto before = context->transferCounts();
                        convert(alias ? *actual : *native, *actual, from, to, 3, true, true, true);
                        require(context->transferCounts() == before, "Vulkan-to-Vulkan conversion staged pixels through CPU.");
                        maximum = std::max(maximum, compare(*readNative(*actual), *expected));
                        ++nativeCases;
                    }

        auto secondDevice = Gpu::VulkanDevice::create(instance, index);
        auto secondContext = std::make_shared<VulkanImageContext>(secondDevice);

        for (unsigned alpha0 = 0; alpha0 < 2; ++alpha0) {
            auto expected = ram(small), initial = ram(small);
            pattern(*expected, 11);
            pattern(*initial, 11);
            initialBitmap(*expected);
            initialBitmap(*initial);
            auto actual = gpu(secondContext, small);
            actual->pasteFrom(*initial, small, true);
            const auto srcBefore = context->transferCounts(), dstBefore = secondContext->transferCounts();
            convert(*input, *expected, eViewerColorSpaceSRGB, eViewerColorSpaceLinear, 3, alpha0, true, true);
            convert(*native, *actual, eViewerColorSpaceSRGB, eViewerColorSpaceLinear, 3, alpha0, true, true);
            require(context->transferCounts()[1] == srcBefore[1] + 1 && secondContext->transferCounts()[0] == dstBefore[0] + 1,
                    "Different-device conversion did not cross the explicit CPU boundary.");
            maximum = std::max(maximum, compare(*readNative(*actual), *expected));
            ++crossContexts;
        }

        require(instance->validationEnabled() && instance->validationErrorCount() == 0, "Conversion produced Vulkan validation errors.");
        std::cout << "{\"ok\":true,\"passed\":true,\"device\":\"" << device->properties().deviceName
                  << "\",\"referenceCases\":" << uploads + downloads + nativeCases + crossContexts
                  << ",\"cpuToVulkanCases\":" << uploads << ",\"vulkanToCpuCases\":" << downloads
                  << ",\"nativeCases\":" << nativeCases << ",\"crossContextCases\":" << crossContexts
                  << ",\"nativeTransfers\":0,\"bitmapAndRoi\":true,\"maxAbsoluteError\":" << maximum << ",\"validationErrors\":0}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
