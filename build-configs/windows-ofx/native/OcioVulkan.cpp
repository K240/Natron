// SPDX-License-Identifier: GPL-2.0-or-later
#include "OcioVulkan.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
namespace OCIO = OCIO_NAMESPACE;
namespace
{
void check(OfxStatus status)
{
    if (status == kOfxStatErrMemory) {
        throw std::bad_alloc();
    }

    if (status != kOfxStatOK) {
        throw std::runtime_error("OCIO Vulkan suite failed: " + std::to_string(status));
    }
}
struct NativeImage {
    const NatronOfxVulkanImageSuiteV1 *suite;
    NatronVulkanContextHandle context;
    NatronVulkanImageHandle handle = 0;
    NativeImage(const NatronOfxVulkanImageSuiteV1* images, NatronVulkanContextHandle ctx, uint32_t width, uint32_t height)
        : suite(images), context(ctx)
    {
        check(suite->createImage(context, width, height, &handle));
    }
    ~NativeImage()
    {
        if (handle) {
            suite->releaseImage(context, handle);
        }
    }
    NativeImage(const NativeImage &) = delete;
    NativeImage &operator=(const NativeImage &) = delete;
};
void validateOutput(const NatronVulkanImageView& output, const OfxRectI& window)
{
    if (!output.writable || window.x2 <= window.x1 || window.y2 <= window.y1 ||
        window.x1 < output.visibleBounds[0] || window.y1 < output.visibleBounds[1] ||
        window.x2 > output.visibleBounds[2] || window.y2 > output.visibleBounds[3]) {
        throw std::invalid_argument("Invalid native IO output permissions/ROI.");
    }
}
std::string linearSampleFunction(unsigned index, uint32_t dimension)
{
    // Hardware filtering may quantize interpolation weights to eight bits.
    // Color transforms use full float weights, including near black and HDR.
    const std::string suffix = dimension == 1 ? "" : std::to_string(dimension);
    const std::string coord = dimension == 1 ? "float" : "vec" + suffix;
    const std::string integer = dimension == 1 ? "int" : "ivec" + suffix;
    std::string code = "vec4 natronSample" + std::to_string(index) + "(sampler" + std::to_string(dimension) + "D lut," + coord + " uv){";
    code += integer + " size=textureSize(lut,0);" + coord + " p=uv*" + coord + "(size)-" + coord + "(.5);";
    code += integer + " base=" + integer + "(floor(p));" + coord + " f=fract(p);vec4 sum=vec4(0.);";

    for (unsigned corner = 0; corner < (1u << dimension); ++corner) {
        code += "sum+=texelFetch(lut,clamp(base+" + integer + "(";

        for (uint32_t axis = 0; axis < dimension; ++axis) {
            if (axis) {
                code += ",";
            }

            code += std::to_string((corner >> axis) & 1u);
        }

        code += ")," + integer + "(0),size-" + integer + "(1)),0)";

        for (uint32_t axis = 0; axis < dimension; ++axis) {
            std::string weight = dimension == 1 ? "f" : std::string("f.") + "xyz"[axis];
            code += "*(" + (((corner >> axis) & 1u) ? weight : "1.-" + weight) + ")";
        }

        code += ";";
    }

    return code + "return sum;}\n";
}
std::vector<unsigned char> uniforms(const OCIO::GpuShaderDescRcPtr &description)
{
    const size_t size = std::max<size_t>(16, (description->getUniformBufferSize() + 15) & ~size_t(15));

    if (size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("OCIO uniform buffer is too large.");
    }

    std::vector<unsigned char> result(size);
    auto write = [&](size_t offset, const void *data, size_t bytes) {
        if (!data || offset > result.size() || bytes > result.size() - offset) {
            throw std::runtime_error("Invalid OCIO uniform layout.");
        }

        std::memcpy(result.data() + offset, data, bytes);
    };

    for (unsigned i = 0; i < description->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        description->getUniform(i, data);

        if (data.m_type == OCIO::UNIFORM_DOUBLE) {
            float value = float(data.m_getDouble());
            write(data.m_bufferOffset, &value, sizeof(value));
        } else if (data.m_type == OCIO::UNIFORM_BOOL) {
            int32_t value = data.m_getBool() ? 1 : 0;
            write(data.m_bufferOffset, &value, sizeof(value));
        } else if (data.m_type == OCIO::UNIFORM_FLOAT3) {
            const auto &value = data.m_getFloat3();
            write(data.m_bufferOffset, value.data(), 3 * sizeof(float));
        } else if (data.m_type == OCIO::UNIFORM_VECTOR_FLOAT || data.m_type == OCIO::UNIFORM_VECTOR_INT) {
            const size_t count = data.m_type == OCIO::UNIFORM_VECTOR_FLOAT ? data.m_vectorFloat.m_getSize() : data.m_vectorInt.m_getSize();
            const void *value = data.m_type == OCIO::UNIFORM_VECTOR_FLOAT ? static_cast<const void *>(data.m_vectorFloat.m_getVector()) : data.m_vectorInt.m_getVector();

            if (count > size / 16) {
                throw std::runtime_error("OCIO uniform array is too large.");
            }

            for (size_t j = 0; j < count; ++j) {
                write(data.m_bufferOffset + j * 16, static_cast<const unsigned char *>(value) + j * 4, 4);
            }
        } else {
            throw std::runtime_error("Unsupported OCIO uniform type.");
        }
    }

    return result;
}
}
struct OcioVulkanProcessor::Impl {
    const NatronOfxVulkanImageSuiteV1 *images;
    const NatronOfxVulkanShaderSuiteV2 *shaders;
    NatronVulkanContextHandle context = 0;
    NatronVulkanProgramHandle program = 0;
    NatronVulkanProgramHandle mipmapProgram = 0, premultProgram = 0, writerAlphaProgram = 0;
    std::string cacheID;
    OCIO::ConstProcessorRcPtr cachedProcessor;
    OCIO::ConstGPUProcessorRcPtr gpuProcessor;
    OCIO::GpuShaderDescRcPtr description;
    mutable std::mutex mutex;
    Impl(const NatronOfxVulkanImageSuiteV1 *i, const NatronOfxVulkanShaderSuiteV2 *s) : images(i), shaders(s)
    {
        if (!images || !shaders) {
            throw std::invalid_argument("OCIO requires Vulkan Image V1 and Shader V2.");
        }

        check(images->createContext(-1, 0, &context));
    }
    ~Impl()
    {
        if (context) {
            images->releaseContext(context);
        }
    }
    bool cachedUnpremult = false, cachedPremult = false;
    void ensureContext()
    {
        if (context) {
            NatronVulkanDeviceInfo info{};
            const auto status = images->getDeviceInfo(context, &info);

            if (status == kOfxStatOK) {
                return;
            }

            if (status != kNatronOfxStatErrDeviceLost) {
                check(status);
            }

            check(images->releaseContext(context));
            context = 0;
            program = mipmapProgram = premultProgram = writerAlphaProgram = 0;
            cacheID.clear();
            cachedProcessor.reset();
            gpuProcessor.reset();
            description.reset();
        }

        check(images->createContext(-1, 0, &context));
    }
    void build(const OCIO::ConstProcessorRcPtr &processor, bool unpremult = false, bool premult = false)
    {
        if (description && cachedProcessor == processor && cacheID == processor->getCacheID() &&
            cachedUnpremult == unpremult && cachedPremult == premult) {
            return;
        }

        auto gpu = processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
        auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
        desc->setFunctionName("NatronOCIO");
        desc->setResourcePrefix("natron_ocio_");
        desc->setDescriptorSetIndex(0, 2); // 0 UBO, 1 source, 2+ immutable LUTs.
        gpu->extractGpuShaderInfo(desc);
        std::vector<NatronVulkanShaderLut> luts;
        std::string shader = desc->getShaderText(), helpers;
        auto linear = [&](const char *samplerName, uint32_t dimensions, OCIO::Interpolation interpolation) {
            if (interpolation == OCIO::INTERP_NEAREST) {
                return;
            }

            const unsigned index = static_cast<unsigned>(luts.size());
            helpers += linearSampleFunction(index, dimensions);
            const std::regex call("texture\\s*\\(\\s*" + std::string(samplerName) + "\\s*,");

            if (!std::regex_search(shader, call)) {
                throw std::runtime_error("Unsupported OCIO linear texture expression.");
            }

            shader = std::regex_replace(shader, call, "natronSample" + std::to_string(index) + "(" + samplerName + ",");
        };

        for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
            const char *textureName = nullptr, *samplerName = nullptr;
            unsigned width = 0, height = 0;
            OCIO::GpuShaderDesc::TextureType channel;
            OCIO::GpuShaderDesc::TextureDimensions dimensions;
            OCIO::Interpolation interpolation;
            desc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interpolation);
            const float *values = nullptr;
            desc->getTextureValues(i, values);
            const uint32_t components = channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL ? 1 : 3;
            linear(samplerName, uint32_t(dimensions), interpolation);
            luts.push_back({desc->getTextureShaderBindingIndex(i), uint32_t(dimensions), width, height, 1, components,
                            0u, values, uint64_t(width) * height * components});
        }

        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char *textureName = nullptr, *samplerName = nullptr;
            unsigned edge = 0;
            OCIO::Interpolation interpolation;
            desc->get3DTexture(i, textureName, samplerName, edge, interpolation);
            const float *values = nullptr;
            desc->get3DTextureValues(i, values);
            linear(samplerName, 3, interpolation);
            luts.push_back({desc->get3DTextureShaderBindingIndex(i), 3, edge, edge, edge, 3,
                            0u, values, uint64_t(edge) * edge * edge * 3});
        }

        NatronVulkanShaderLayout layout{0, 1, {1, 0, 0, 0}, uint32_t(luts.size()), luts.data()};
        std::string source = "#version 460\nlayout(set=0,binding=1) uniform sampler2D natronSource;\n";
        source += helpers + shader;
        source += "\nlayout(location=0) out vec4 natronOutput;\nvoid main(){vec4 value=texelFetch(natronSource,ivec2(gl_FragCoord.xy),0);";

        if (unpremult) {
            source += "if(value.a>1.1920928955078125e-7)value.rgb/=value.a;";
        }

        source += "value=NatronOCIO(value);";

        // Match ofxsUnPremult/ofxsPremult, including nonpositive/NaN alpha.
        if (premult) {
            source += "float alpha=value.a>0.?value.a:0.;value=vec4(value.rgb*alpha,alpha);";
        }

        source += "natronOutput=value;}\n";
        auto values = uniforms(desc);
        char error[8192] {};
        NatronVulkanProgramHandle next = 0;
        OfxStatus status = shaders->createProgram(context, source.c_str(), uint32_t(values.size()), &layout, &next, error, sizeof(error));

        if (status != kOfxStatOK) {
            if (status == kOfxStatErrMemory) {
                check(status);
            }

            throw std::runtime_error(std::string("OCIO Vulkan compile failed: ") + error);
        }

        if (program) {
            check(shaders->releaseProgram(context, program));
        }

        program = next;
        description = std::move(desc);
        gpuProcessor = std::move(gpu);
        cacheID = processor->getCacheID();
        cachedProcessor = processor;
        cachedUnpremult = unpremult;
        cachedPremult = premult;
    }
    NatronVulkanProgramHandle auxiliary(NatronVulkanProgramHandle& handle, const char* body)
    {
        if (!handle) {
            const std::string source = std::string("#version 460\nlayout(set=0,binding=1) uniform sampler2D natronSource;\n") +
                                       "layout(std140,set=0,binding=0) uniform Parameters{ivec4 origins;};\nlayout(location=0) out vec4 result;\n" + body;
            const NatronVulkanShaderLayout layout{0, 1, {1, 0, 0, 0}, 0, nullptr};
            char error[8192] {};
            const OfxStatus status = shaders->createProgram(context, source.c_str(), 16, &layout, &handle, error, sizeof(error));

            if (status != kOfxStatOK) {
                if (status == kOfxStatErrMemory) {
                    check(status);
                }

                throw std::runtime_error(std::string("Native IO Vulkan compile failed: ") + error);
            }
        }

        return handle;
    }
};
OcioVulkanProcessor::OcioVulkanProcessor(const NatronOfxVulkanImageSuiteV1 *images, const NatronOfxVulkanShaderSuiteV2 *shaders)
    : _imp(new Impl(images, shaders)) {}
OcioVulkanProcessor::~OcioVulkanProcessor() = default;
NatronVulkanDeviceInfo OcioVulkanProcessor::deviceInfo() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanDeviceInfo info{};
    check(_imp->images->getDeviceInfo(_imp->context, &info));
    return info;
}

void OcioVulkanProcessor::downloadWriter(const OCIO::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
        OfxPropertySetHandle source, const OfxRectI& window, float *pixels, int64_t rowBytes,
        bool opaque, bool unpremult, bool premult)
{
    const int64_t width = int64_t(window.x2) - window.x1, height = int64_t(window.y2) - window.y1;

    if (!pixels || width <= 0 || height <= 0 || width > UINT32_MAX || height > UINT32_MAX ||
        rowBytes == INT64_MIN || (rowBytes < 0 ? -rowBytes : rowBytes) < width * 16 || (opaque && unpremult)) {
        throw std::invalid_argument("Invalid native Writer buffer/alpha operation.");
    }

    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanImageView input{};
    check(render.acquireImage(_imp->context, source, &input));

    if (input.writable || window.x1 < input.visibleBounds[0] || window.y1 < input.visibleBounds[1] ||
        window.x2 > input.visibleBounds[2] || window.y2 > input.visibleBounds[3]) {
        throw std::invalid_argument("Invalid native Writer input permissions/ROI.");
    }

    NativeImage processed(_imp->images, _imp->context, uint32_t(width), uint32_t(height));
    const NatronVulkanRegion roi{uint32_t(window.x1 - input.bounds[0]), uint32_t(window.y1 - input.bounds[1]),
                                 uint32_t(width), uint32_t(height)};
    check(_imp->images->copy(_imp->context, input.image, processed.handle, &roi, 0, 0));
    const NatronVulkanShaderInput inputs[4] {{processed.handle, kNatronVulkanWrapClamp, 0}, {}, {}, {}};

    if (opaque || unpremult) {
        const auto shader = _imp->auxiliary(_imp->writerAlphaProgram,
                                            "void main(){vec4 value=texelFetch(natronSource,ivec2(gl_FragCoord.xy),0);"
                                            "if(origins.x==1)value.a=1.;else if(value.a>1.1920928955078125e-7)value.rgb/=value.a;result=value;}");
        const int32_t operation[4] {opaque ? 1 : 2, 0, 0, 0};
        check(_imp->shaders->render(_imp->context, shader, processed.handle, nullptr, inputs, operation, sizeof(operation)));
    }

    if (processor && !processor->isNoOp()) {
        _imp->build(processor, false, false);
        auto values = uniforms(_imp->description);
        check(_imp->shaders->render(_imp->context, _imp->program, processed.handle, nullptr, inputs, values.data(), uint32_t(values.size())));
    }

    if (premult) {
        const auto shader = _imp->auxiliary(_imp->premultProgram,
                                            "void main(){vec4 value=texelFetch(natronSource,ivec2(gl_FragCoord.xy),0);"
                                            "float alpha=value.a>0.?value.a:0.;result=vec4(value.rgb*alpha,alpha);}");
        const int32_t operation[4] {};
        check(_imp->shaders->render(_imp->context, shader, processed.handle, nullptr, inputs, operation, sizeof(operation)));
    }

    check(_imp->images->download(_imp->context, processed.handle, pixels, rowBytes));
}

void OcioVulkanProcessor::copyClip(const NatronOfxVulkanRenderSuiteV1& render, OfxPropertySetHandle source,
                                   OfxPropertySetHandle destination, const OfxRectI& window)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanImageView input{}, output{};
    check(render.acquireImage(_imp->context, source, &input));
    check(render.acquireImage(_imp->context, destination, &output));
    validateOutput(output, window);

    if (input.writable || window.x1 < input.visibleBounds[0] || window.y1 < input.visibleBounds[1] ||
        window.x2 > input.visibleBounds[2] || window.y2 > input.visibleBounds[3]) {
        throw std::invalid_argument("Invalid native Writer pass-through input.");
    }

    const NatronVulkanRegion roi{uint32_t(window.x1 - input.bounds[0]), uint32_t(window.y1 - input.bounds[1]),
                                 uint32_t(int64_t(window.x2) - window.x1), uint32_t(int64_t(window.y2) - window.y1)};
    check(_imp->images->copy(_imp->context, input.image, output.image, &roi,
                             window.x1 - output.bounds[0], window.y1 - output.bounds[1]));
}
std::array<uint32_t, 5> OcioVulkanProcessor::resourceInfo() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    std::array<uint32_t, 5> info{};

    if (!_imp->description) {
        return info;
    }

    auto desc = _imp->description;

    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char *texture = nullptr, *sampler = nullptr;
        unsigned width = 0, height = 0;
        OCIO::GpuShaderDesc::TextureType channel;
        OCIO::GpuShaderDesc::TextureDimensions dimension;
        OCIO::Interpolation interpolation;
        desc->getTexture(i, texture, sampler, width, height, channel, dimension, interpolation);
        ++info[uint32_t(dimension) - 1];
    }

    info[2] = desc->getNum3DTextures();
    info[3] = desc->getNumUniforms();

    for (unsigned i = 0; i < info[3]; ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        desc->getUniform(i, data);

        if (data.m_type <= OCIO::UNIFORM_VECTOR_INT) {
            info[4] |= 1u << uint32_t(data.m_type);
        }
    }

    return info;
}
bool OcioVulkanProcessor::tryApply(const OCIO::ConstProcessorRcPtr &processor, float *pixels, uint32_t width, uint32_t height,
                                   uint32_t components, int64_t rowBytes, NatronVulkanDeviceInfo *info) noexcept
{
    try {
        // Query before applying: a failed query after committing pixels would
        // send already converted pixels through the CPU transform a second time.
        const auto device = deviceInfo();
        apply(processor, pixels, width, height, components, rowBytes);

        if (info) {
            *info = device;
        }

        return true;
    } catch (...) {
        // Never unwind through an OFX CPU worker callback. apply() commits the
        // packed result only after every suite operation has succeeded.
        return false;
    }
}
void OcioVulkanProcessor::apply(const OCIO::ConstProcessorRcPtr &processor, float *pixels, uint32_t width, uint32_t height,
                                uint32_t components, int64_t rowBytes)
{
    if (!processor || !pixels || !width || !height || (components != 3 && components != 4) || rowBytes == INT64_MIN ||
        uint64_t(rowBytes < 0 ? -rowBytes : rowBytes) < uint64_t(width) * components * sizeof(float) ||
        uint64_t(width) * height > std::numeric_limits<size_t>::max() / (4 * sizeof(float)) ||
        uint64_t(height - 1) > uint64_t(PTRDIFF_MAX) / uint64_t(rowBytes < 0 ? -rowBytes : rowBytes)) {
        throw std::invalid_argument("Invalid OCIO Vulkan pixel buffer or row stride.");
    }

    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    _imp->build(processor);
    std::vector<float> packed(size_t(width) * height * 4, 1.f);

    for (uint32_t y = 0; y < height; ++y) {
        const auto *row = reinterpret_cast<const float *>(reinterpret_cast<const char *>(pixels) + int64_t(y) * rowBytes);

        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(packed.data() + (size_t(y) * width + x) * 4, row + size_t(x) * components, components * sizeof(float));
        }
    }

    NatronVulkanImageHandle image = 0;
    check(_imp->images->createImage(_imp->context, width, height, &image));

    try {
        check(_imp->images->upload(_imp->context, image, packed.data(), uint64_t(width) * 4 * sizeof(float)));
        auto values = uniforms(_imp->description);
        NatronVulkanShaderInput inputs[4] {{image, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
        check(_imp->shaders->render(_imp->context, _imp->program, image, nullptr, inputs, values.data(), uint32_t(values.size())));
        check(_imp->images->download(_imp->context, image, packed.data(), uint64_t(width) * 4 * sizeof(float)));
        check(_imp->images->releaseImage(_imp->context, image));
    } catch (...) {
        _imp->images->releaseImage(_imp->context, image);
        throw;
    }

    for (uint32_t y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<float *>(reinterpret_cast<char *>(pixels) + int64_t(y) * rowBytes);

        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(row + size_t(x) * components, packed.data() + (size_t(y) * width + x) * 4, components * sizeof(float));
        }
    }
}

bool OcioVulkanProcessor::applyClip(const OCIO::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
                                    OfxPropertySetHandle source, OfxPropertySetHandle destination, const OfxRectI& window,
                                    bool premult, float mix, bool masked, bool inverted, const float *maskRGBA)
{
    if (!processor || window.x2 <= window.x1 || window.y2 <= window.y1) {
        throw std::invalid_argument("Invalid native OCIO clip render.");
    }

    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanImageView input{}, output{};
    check(render.acquireImage(_imp->context, source, &input));
    check(render.acquireImage(_imp->context, destination, &output));

    if (input.writable || !output.writable || window.x1 < output.visibleBounds[0] || window.y1 < output.visibleBounds[1] ||
        window.x2 > output.visibleBounds[2] || window.y2 > output.visibleBounds[3]) {
        throw std::invalid_argument("Invalid native OCIO clip permissions/ROI.");
    }

    struct Image {
        const NatronOfxVulkanImageSuiteV1 *suite;
        NatronVulkanContextHandle context;
        NatronVulkanImageHandle handle = 0;
        Image(const NatronOfxVulkanImageSuiteV1* images, NatronVulkanContextHandle ctx, uint32_t w, uint32_t h) : suite(images), context(ctx)
        {
            check(suite->createImage(context, w, h, &handle));
        }
        ~Image()
        {
            if (handle) {
                suite->releaseImage(context, handle);
            }
        }
    };

    const uint32_t width = window.x2 - window.x1, height = window.y2 - window.y1;

    // With matching full-image grids and no mix/mask, the immutable input can
    // be sampled directly into the borrowed output. Partial/shifted grids still
    // use the zero-padded temporary images below to preserve their semantics.
    const auto matchesWindow = [&](const int32_t * bounds) {
        return bounds[0] == window.x1 && bounds[1] == window.y1 &&
               bounds[2] == window.x2 && bounds[3] == window.y2;
    };

    if (!masked && mix == 1.f && input.image != output.image &&
        matchesWindow(input.bounds) && matchesWindow(input.visibleBounds) &&
        matchesWindow(output.bounds) && matchesWindow(output.visibleBounds)) {
        _imp->build(processor, premult, premult);
        auto values = uniforms(_imp->description);
        const NatronVulkanShaderInput inputs[4] {{input.image, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
        check(_imp->shaders->render(_imp->context, _imp->program, output.image, nullptr,
                                    inputs, values.data(), uint32_t(values.size())));
        return true;
    }

    Image original(_imp->images, _imp->context, width, height), processed(_imp->images, _imp->context, width, height);

    const int x1 = std::max(window.x1, input.visibleBounds[0]), y1 = std::max(window.y1, input.visibleBounds[1]);

    const int x2 = std::min(window.x2, input.visibleBounds[2]), y2 = std::min(window.y2, input.visibleBounds[3]);

    if (x2 > x1 && y2 > y1) {
        const NatronVulkanRegion roi{uint32_t(x1 - input.bounds[0]), uint32_t(y1 - input.bounds[1]), uint32_t(x2 - x1), uint32_t(y2 - y1)};
        check(_imp->images->copy(_imp->context, input.image, original.handle, &roi, x1 - window.x1, y1 - window.y1));
    }

    _imp->build(processor, premult, premult);
    auto values = uniforms(_imp->description);
    NatronVulkanShaderInput inputs[4] {{original.handle, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
    check(_imp->shaders->render(_imp->context, _imp->program, processed.handle, nullptr, inputs, values.data(), uint32_t(values.size())));
    std::unique_ptr<Image> mask;

    if (masked && maskRGBA) {
        mask = std::make_unique<Image>(_imp->images, _imp->context, width, height);
        check(_imp->images->upload(_imp->context, mask->handle, maskRGBA, uint64_t(width) * 16));
    }

    if (masked || mix != 1.f) {
        check(_imp->images->mix(_imp->context, processed.handle, original.handle, mask ? mask->handle : 0,
                                processed.handle, nullptr, mix, masked ? 1 : 0, inverted ? 1 : 0));
    }

    check(_imp->images->copy(_imp->context, processed.handle, output.image, nullptr,
                             window.x1 - output.bounds[0], window.y1 - output.bounds[1]));
    return false;
}

void OcioVulkanProcessor::clearClip(const NatronOfxVulkanRenderSuiteV1& render, OfxPropertySetHandle destination,
                                    const OfxRectI& window)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanImageView output{};
    check(render.acquireImage(_imp->context, destination, &output));
    validateOutput(output, window);
    NativeImage zero(_imp->images, _imp->context, uint32_t(int64_t(window.x2) - window.x1), uint32_t(int64_t(window.y2) - window.y1));
    check(_imp->images->copy(_imp->context, zero.handle, output.image, nullptr,
                             window.x1 - output.bounds[0], window.y1 - output.bounds[1]));
}

void OcioVulkanProcessor::uploadReader(const OCIO::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
                                       const float *pixels, const OfxRectI& bounds, int64_t rowBytes, const OfxRectI& crop,
                                       OfxPropertySetHandle destination, const OfxRectI& window, unsigned levels,
                                       bool unpremult, bool premult)
{
    const int64_t width = int64_t(crop.x2) - crop.x1, height = int64_t(crop.y2) - crop.y1;

    if (!pixels || width <= 0 || height <= 0 || width > UINT32_MAX || height > UINT32_MAX || levels > 30 ||
        crop.x1 < bounds.x1 || crop.y1 < bounds.y1 || crop.x2 > bounds.x2 || crop.y2 > bounds.y2 ||
        rowBytes == INT64_MIN || (rowBytes < 0 ? -rowBytes : rowBytes) < (int64_t(bounds.x2) - bounds.x1) * 16 ||
        (unpremult && (!processor || processor->isNoOp()))) {
        throw std::invalid_argument("Invalid native Reader buffer/crop.");
    }

    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->ensureContext();
    NatronVulkanImageView output{};
    check(render.acquireImage(_imp->context, destination, &output));
    validateOutput(output, window);
    auto current = std::make_unique<NativeImage>(_imp->images, _imp->context, uint32_t(width), uint32_t(height));
    const auto cropped = reinterpret_cast<const unsigned char *>(pixels) + int64_t(crop.y1 - bounds.y1) * rowBytes +
                         int64_t(crop.x1 - bounds.x1) * 16;
    check(_imp->images->upload(_imp->context, current->handle, cropped, rowBytes));
    OfxRectI currentBounds = crop;

    if (processor && !processor->isNoOp()) {
        _imp->build(processor, unpremult, false);
        auto values = uniforms(_imp->description);
        const NatronVulkanShaderInput inputs[4] {{current->handle, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
        check(_imp->shaders->render(_imp->context, _imp->program, current->handle, nullptr, inputs, values.data(), uint32_t(values.size())));
    }

    for (unsigned i = 0; i < levels; ++i) {
        const auto floorHalf = [](int value) {
            return value / 2 - (value < 0 && value % 2 != 0);
        };
        const auto ceilHalf = [](int value) {
            return value / 2 + (value > 0 && value % 2 != 0);
        };
        const OfxRectI nextBounds{floorHalf(currentBounds.x1), floorHalf(currentBounds.y1), ceilHalf(currentBounds.x2), ceilHalf(currentBounds.y2)};
        auto next = std::make_unique<NativeImage>(_imp->images, _imp->context, nextBounds.x2 - nextBounds.x1, nextBounds.y2 - nextBounds.y1);
        const auto shader = _imp->auxiliary(_imp->mipmapProgram,
                                            "void main(){ivec2 p=(ivec2(gl_FragCoord.xy)+origins.zw)*2-origins.xy;"
                                            "ivec2 size=textureSize(natronSource,0);vec4 sum=vec4(0.);int count=0;"
                                            "for(int y=0;y<2;++y)for(int x=0;x<2;++x){ivec2 q=p+ivec2(x,y);"
                                            "if(all(greaterThanEqual(q,ivec2(0)))&&all(lessThan(q,size))){sum+=texelFetch(natronSource,q,0);++count;}}"
                                            "result=count>0?sum/float(count):vec4(0.);}");
        const int32_t origins[4] {currentBounds.x1, currentBounds.y1, nextBounds.x1, nextBounds.y1};
        const NatronVulkanShaderInput inputs[4] {{current->handle, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
        check(_imp->shaders->render(_imp->context, shader, next->handle, nullptr, inputs, origins, sizeof(origins)));
        current = std::move(next);
        currentBounds = nextBounds;
    }

    if (window.x1 < currentBounds.x1 || window.y1 < currentBounds.y1 || window.x2 > currentBounds.x2 || window.y2 > currentBounds.y2) {
        throw std::invalid_argument("Native Reader crop does not cover output ROI.");
    }

    if (premult) {
        const auto shader = _imp->auxiliary(_imp->premultProgram,
                                            "void main(){vec4 value=texelFetch(natronSource,ivec2(gl_FragCoord.xy),0);"
                                            "float alpha=value.a>0.?value.a:0.;result=vec4(value.rgb*alpha,alpha);}");
        const int32_t origins[4] {};
        const NatronVulkanShaderInput inputs[4] {{current->handle, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
        check(_imp->shaders->render(_imp->context, shader, current->handle, nullptr, inputs, origins, sizeof(origins)));
    }

    const NatronVulkanRegion roi{uint32_t(window.x1 - currentBounds.x1), uint32_t(window.y1 - currentBounds.y1),
                                 uint32_t(int64_t(window.x2) - window.x1), uint32_t(int64_t(window.y2) - window.y1)};
    check(_imp->images->copy(_imp->context, current->handle, output.image, &roi,
                             window.x1 - output.bounds[0], window.y1 - output.bounds[1]));
}
