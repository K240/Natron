// SPDX-License-Identifier: GPL-2.0-or-later
#include "Shadertoy.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "ofxsCoords.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
namespace Shadertoy
{
constexpr unsigned NBINPUTS = SHADERTOY_NBINPUTS;
namespace
{
std::atomic<uint64_t> nativeRenders{0};
std::atomic<uint64_t> clipRenders{0};
std::mutex infoMutex;
NatronVulkanDeviceInfo lastInfo{};
void check(OfxStatus status)
{
    if (status != kOfxStatOK) {
        OFX::throwSuiteStatusException(status);
    }
}
const NatronOfxVulkanImageSuiteV1 *images()
{
    return static_cast<const NatronOfxVulkanImageSuiteV1 *>(OFX::fetchSuite(kNatronOfxVulkanImageSuite, 1, true));
}
const NatronOfxVulkanShaderSuiteV1 *shaders()
{
    return static_cast<const NatronOfxVulkanShaderSuiteV1 *>(OFX::fetchSuite(kNatronOfxVulkanShaderSuite, 1, true));
}
const NatronOfxVulkanRenderSuiteV1 *renderSuite()
{
    return static_cast<const NatronOfxVulkanRenderSuiteV1 *>(OFX::fetchSuite(kNatronOfxVulkanRenderSuite, 1, true));
}
std::string withoutComments(std::string source)
{
    bool line = false, block = false;

    for (size_t i = 0; i < source.size(); ++i) {
        if (line && source[i] == '\n') {
            line = false;
        }

        if (block && source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/') {
            source[i] = source[i + 1] = ' ';
            ++i;
            block = false;
            continue;
        }

        if (!line && !block && source[i] == '/' && i + 1 < source.size()) {
            if (source[i + 1] == '/') {
                line = true;
            } else if (source[i + 1] == '*') {
                block = true;
            }
        }

        if ((line || block) && source[i] != '\n' && source[i] != '\r') {
            source[i] = ' ';
        }
    }

    return source;
}
struct Uniform {
    std::string type, name;
    unsigned slot;
};
struct Source {
    std::string glsl, code;
    std::vector<Uniform> extra;
    unsigned bytes;
};
Source translate(const std::string& original)
{
    Source result;
    result.code = withoutComments(original);
    std::string body = original;
    const std::regex declarations(R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;)"),
          compatibility(R"((^|\n)[ \t]*(?:#version[^\n]*|#extension[ \t]+GL_OES_standard_derivatives[^\n]*|const[ \t]+vec2[ \t]+i(?:RenderScale|ChannelOffset)[^\n]*))"),
          precision(R"(\bprecision\s+(?:lowp|mediump|highp)\s+\w+\s*;)");
    const std::map<std::string, unsigned> builtins{{"iResolution", 0}, {"iTime", 1}, {"iGlobalTime", 2}, {"iTimeDelta", 3},
        {"iFrame", 4}, {"iMouse", 5}, {"iDate", 6}, {"iSampleRate", 7}, {"ifFragCoordOffsetUniform", 8}, {"iRenderScale", 9},
        {"iChannelResolution", 10}, {"iChannelTime", 14}, {"iChannelOffset", 18}};
    unsigned slot = 22;
    auto clearRange = [&](size_t start, size_t count) {
        for (size_t i = start; i < start + count; ++i) if (body[i] != '\n' && body[i] != '\r') {
                body[i] = ' ';
            }
    };

    for (std::sregex_iterator it(result.code.begin(), result.code.end(), declarations), end; it != end; ++it) {
        const std::string type = (*it)[1], name = (*it)[2];

        if (!builtins.count(name) && name != "iChannel0" && name != "iChannel1" && name != "iChannel2" && name != "iChannel3") {
            if (type != "bool" && type != "int" && type != "float" && type != "vec2" && type != "vec3" && type != "vec4") {
                throw std::runtime_error("Unsupported Shadertoy uniform type: " + type);
            }

            if ((*it).str().find('[') != std::string::npos) {
                throw std::runtime_error("Shadertoy extra uniform arrays are unsupported.");
            }

            result.extra.push_back({type, name, slot++});
        }

        clearRange(it->position(), it->length());
    }

    for (std::sregex_iterator it(result.code.begin(), result.code.end(), compatibility), end; it != end; ++it) {
        clearRange(it->position(), it->length());
    }

    for (std::sregex_iterator it(result.code.begin(), result.code.end(), precision), end; it != end; ++it) {
        clearRange(it->position(), it->length());
    }

    std::ostringstream prefix;
    prefix << "#version 450\n#define lowp\n#define mediump\n#define highp\n"
           << "#define texture2D texture\n#define texture2DLod textureLod\n#define texture2DGrad textureGrad\n#define texture2DProj textureProj\n"
           << "layout(location=0) out vec4 natronColor;\n#define gl_FragColor natronColor\n";

    for (unsigned i = 0; i < 4; ++i) {
        prefix << "layout(set=0,binding=" << i << ") uniform sampler2D iChannel" << i << ";\n";
    }

    prefix << "layout(set=0,binding=4,std140) uniform NatronUniforms {\n"
           << "layout(offset=0) vec3 iResolution;\nlayout(offset=16) float iTime;\nlayout(offset=32) float iGlobalTime;\n"
           << "layout(offset=48) float iTimeDelta;\nlayout(offset=64) int iFrame;\nlayout(offset=80) vec4 iMouse;\n"
           << "layout(offset=96) vec4 iDate;\nlayout(offset=112) float iSampleRate;\n"
           << "layout(offset=128) vec2 ifFragCoordOffsetUniform;\nlayout(offset=144) vec2 iRenderScale;\n"
           << "layout(offset=160) vec3 iChannelResolution[4];\nlayout(offset=224) float iChannelTime[4];\n"
           << "layout(offset=288) vec2 iChannelOffset[4];\n";

    for (const auto& uniform : result.extra) {
        prefix << "layout(offset=" << uniform.slot * 16 << ") " << uniform.type << ' ' << uniform.name << ";\n";
    }

    prefix << "};\n#line 1\n" << body << "\nvoid main(){mainImage(natronColor,gl_FragCoord.xy+ifFragCoordOffsetUniform);}\n";
    result.glsl = prefix.str();
    result.bytes = slot * 16;
    return result;
}
struct ImageHandle {
    const NatronOfxVulkanImageSuiteV1 *suite;
    NatronVulkanContextHandle context;
    NatronVulkanImageHandle handle = 0;
    ImageHandle(const NatronOfxVulkanImageSuiteV1* value, NatronVulkanContextHandle owner, unsigned w, unsigned h) : suite(value), context(owner)
    {
        check(suite->createImage(context, w, h, &handle));
    }
    ~ImageHandle()
    {
        if (handle) {
            suite->releaseImage(context, handle);
        }
    }
};
float read(const OFX::Image& image, int x, int y, unsigned channel)
{
    const void *pixel = image.getPixelAddress(x, y);

    if (!pixel) {
        return 0;
    }

    switch (image.getPixelDepth()) {
    case OFX::eBitDepthUByte:
        return static_cast<const unsigned char *>(pixel)[channel] / 255.f;

    case OFX::eBitDepthUShort:
        return static_cast<const unsigned short *>(pixel)[channel] / 65535.f;

    case OFX::eBitDepthFloat:
        return static_cast<const float *>(pixel)[channel];

    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        return 0;
    }
}
std::vector<float> pack(const OFX::Image& image)
{
    const auto b = image.getBounds();
    std::vector<float> result(size_t(b.x2 - b.x1) * (b.y2 - b.y1) * 4);
    unsigned components = image.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : image.getPixelComponents() == OFX::ePixelComponentRGB ? 3 : 1;

    for (int y = b.y1; y < b.y2; ++y) for (int x = b.x1; x < b.x2; ++x) {
            float *out = result.data() + (size_t(y - b.y1) * (b.x2 - b.x1) + x - b.x1) * 4;

            if (components == 1) {
                out[3] = read(image, x, y, 0);
            } else {
                for (unsigned c = 0; c < components; ++c) {
                    out[c] = read(image, x, y, c);
                }

                if (components == 3) {
                    out[3] = 1;
                }
            }
        }

    return result;
}
void unpack(OFX::Image& image, const OfxRectI& roi, const std::vector<float> &values)
{
    auto b = image.getBounds();
    unsigned components = image.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : image.getPixelComponents() == OFX::ePixelComponentRGB ? 3 : 1;

    for (int y = roi.y1; y < roi.y2; ++y) for (int x = roi.x1; x < roi.x2; ++x) {
            void *pixel = image.getPixelAddress(x, y);

            if (!pixel) {
                continue;
            }

            const float *in = values.data() + (size_t(y - b.y1) * (b.x2 - b.x1) + x - b.x1) * 4;

            for (unsigned c = 0; c < components; ++c) {
                float value = in[components == 1 ? 3 : c];

                switch (image.getPixelDepth()) {
                case OFX::eBitDepthUByte:
                    static_cast<unsigned char *>(pixel)[c] = std::lround(std::clamp(value, 0.f, 1.f) * 255.f);
                    break;

                case OFX::eBitDepthUShort:
                    static_cast<unsigned short *>(pixel)[c] = std::lround(std::clamp(value, 0.f, 1.f) * 65535.f);
                    break;

                case OFX::eBitDepthFloat:
                    static_cast<float *>(pixel)[c] = value;
                    break;

                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                }
            }
        }
}
}
struct NativeVulkanShaderData {
    const NatronOfxVulkanImageSuiteV1 *imageSuite = images();
    const NatronOfxVulkanShaderSuiteV1 *shaderSuite = shaders();
    NatronVulkanContextHandle context = 0;
    NatronVulkanProgramHandle program = 0;
    std::string compiledSource;
    unsigned shaderId = 0;
    NativeVulkanShaderData()
    {
        check(imageSuite->createContext(-1, 0, &context));
    }
    ~NativeVulkanShaderData()
    {
        if (context) {
            imageSuite->releaseContext(context);
        }
    }
};
bool ShadertoyPlugin::hasVulkan()
{
    return images() && shaders();
}
bool ShadertoyPlugin::hasVulkanClips()
{
    return hasVulkan() && renderSuite();
}
void ShadertoyPlugin::enableVulkanClips()
{
    getPropertySet().propSetInt(kNatronOfxImageEffectPropVulkanRenderSupported, hasVulkanClips() ? 1 : 0, 0, false);
}
void ShadertoyPlugin::renderVulkan(const OFX::RenderArguments& args)
{
    bool update = false;

    try {
        {
            AutoMutex lock(_imageShaderMutex.get());

            if (!_vulkanShader) {
                _vulkanShader = std::make_shared<NativeVulkanShaderData>();
            } else {
                NatronVulkanDeviceInfo info{};
                const auto status = _vulkanShader->imageSuite->getDeviceInfo(_vulkanShader->context, &info);

                if (status == kNatronOfxStatErrDeviceLost) {
                    _vulkanShader = std::make_shared<NativeVulkanShaderData>();
                } else {
                    check(status);
                }
            }

            auto& state = *_vulkanShader;
            std::string source;
            _imageShaderSource->getValue(source);
            Source shader = translate(source);
            bool recompiled = state.compiledSource != shader.glsl || state.shaderId != _imageShaderID || _imageShaderUpdateParams;

            if (recompiled) {
                NatronVulkanProgramHandle program = 0;
                std::array<char, 8192> error{};
                OfxStatus status = state.shaderSuite->createProgram(state.context, shader.glsl.c_str(), shader.bytes, &program, error.data(), error.size());

                if (status != kOfxStatOK) {
                    setPersistentMessage(OFX::Message::eMessageError, "", std::string("Vulkan shader compilation failed:\n") + error.data());
                    check(status);
                }

                if (state.program) {
                    check(state.shaderSuite->releaseProgram(state.context, state.program));
                }

                state.program = program;
                state.compiledSource = shader.glsl;
                state.shaderId = _imageShaderID;
                _imageShaderCompiled = true;

                if (_imageShaderUpdateParams) {
                    uint32_t samplerMask = 0, uniformCount = 0;
                    check(state.shaderSuite->getResources(state.context, state.program, &samplerMask, &uniformCount, nullptr, 0));
                    std::vector<uint32_t> active(uniformCount);
                    check(state.shaderSuite->getResources(state.context, state.program, &samplerMask, &uniformCount, active.data(), uniformCount));
                    _imageShaderExtraParameters.clear();
                    _imageShaderHasMouse = std::binary_search(active.begin(), active.end(), 80u);

                    for (unsigned i = 0; i < NBINPUTS; ++i) {
                        _imageShaderInputEnabled[i] = (samplerMask & (1u << i)) != 0;
                        getChannelInfo(source.c_str(), i, _imageShaderInputLabel[i], _imageShaderInputHint[i], _imageShaderInputFilter[i], _imageShaderInputWrap[i]);
                    }

                    for (const auto& uniform : shader.extra) {
                        if (!std::binary_search(active.begin(), active.end(), uniform.slot * 16)) {
                            continue;
                        }

                        ExtraParameter parameter;
                        UniformTypeEnum type = uniform.type == "bool" ? eUniformTypeBool : uniform.type == "int" ? eUniformTypeInt :
                                               uniform.type == "float" ? eUniformTypeFloat : uniform.type == "vec2" ? eUniformTypeVec2 : uniform.type == "vec3" ? eUniformTypeVec3 : eUniformTypeVec4;
                        parameter.init(type, uniform.name);
                        getExtraParameterInfo(source.c_str(), parameter);
                        _imageShaderExtraParameters.push_back(parameter);
                    }

                    std::sort(_imageShaderExtraParameters.begin(), _imageShaderExtraParameters.end(), ExtraParameter::less_than_pos());
                    _imageShaderBBox = static_cast<BBoxEnum>(_bbox->getValueAtTime(args.time));
                    getBboxInfo(source.c_str(), _imageShaderBBox);
                    update = true;
                }
            }

            std::unique_ptr<OFX::Image> destination(_dstClip->fetchImage(args.time));

            if (!destination) {
                OFX::throwSuiteStatusException(kOfxStatFailed);
            }

            auto b = destination->getBounds();
            OfxRectI region{std::max(b.x1, args.renderWindow.x1), std::max(b.y1, args.renderWindow.y1),
                            std::min(b.x2, args.renderWindow.x2), std::min(b.y2, args.renderWindow.y2)};

            if (region.x2 <= region.x1 || region.y2 <= region.y1 || abort()) {
                return;
            }

            const bool nativeClips = !destination->getPixelData();
            NatronVulkanImageView nativeOutput{};

            if (nativeClips) {
                if (!hasVulkanClips() || destination->getPixelDepth() != OFX::eBitDepthFloat || destination->getPixelComponentCount() != 4) {
                    OFX::throwSuiteStatusException(kOfxStatErrFormat);
                }

                check(renderSuite()->acquireImage(state.context, destination->getPropertySet().propSetHandle(), &nativeOutput));
                b = {nativeOutput.bounds[0], nativeOutput.bounds[1], nativeOutput.bounds[2], nativeOutput.bounds[3]};
            }

            OfxRectI full;
            OFX::Coords::toPixelEnclosing(_dstClip->getRegionOfDefinition(args.time), args.renderScale, _dstClip->getPixelAspectRatio(), &full);
            using Slot = std::array<uint32_t, 4>;
            std::vector<Slot> uniforms(shader.bytes / 16);
            auto floats = [&](unsigned slot, std::initializer_list<float> values) {
                unsigned component = 0;

                for (float value : values) {
                    std::memcpy(&uniforms[slot][component++], &value, sizeof(value));
                }
            };
            const double fps = _dstClip->getFrameRate() > 0 ? _dstClip->getFrameRate() : 1.;
            float time = args.time / fps;
            floats(0, {float(full.x2 - full.x1), float(full.y2 - full.y1), 1});
            floats(1, {time});
            floats(2, {time});
            floats(3, {float(1 / fps)});
            int frame = args.time;
            std::memcpy(uniforms[4].data(), &frame, sizeof(frame));
            double mx = 0, my = 0, cx = 0, cy = 0;

            if (_mouseParams->getValueAtTime(args.time)) {
                _mousePosition->getValueAtTime(args.time, mx, my);
                _mouseClick->getValueAtTime(args.time, cx, cy);

                if (!_mousePressed->getValueAtTime(args.time)) {
                    cx = -cx;
                    cy = -cy;
                }
            }

            floats(5, {float(mx * args.renderScale.x), float(my * args.renderScale.y), float(cx * args.renderScale.x), float(cy * args.renderScale.y)});
            double year, month, day, seconds;
            _date->getValueAtTime(args.time, year, month, day, seconds);
            year = std::floor(year);
            month = std::floor(month);
            day = std::floor(day);
            seconds += time;
            day += std::floor(seconds / 86400);
            seconds -= std::floor(seconds / 86400) * 86400;
            int days = month == 1 ? 28 : month == 3 || month == 5 || month == 8 || month == 10 ? 30 : 31;

            if (day > days) {
                day -= days;
                month = (int(month) + 1) % 12;
            }

            floats(6, {float(year), float(month), float(day), float(seconds)});
            floats(7, {44100});
            floats(8, {float(b.x1 - full.x1), float(b.y1 - full.y1)});
            floats(9, {float(args.renderScale.x), float(args.renderScale.y)});
            std::array<std::unique_ptr<OFX::Image>, 4> sources;
            std::array<std::unique_ptr<ImageHandle>, 4> nativeSources;
            std::array<NatronVulkanShaderInput, 4> inputs{};
            std::array<OfxRectI, 4> sourceBounds{};

            for (unsigned i = 0; i < NBINPUTS; ++i) {
                if (_inputEnable[i]->getValueAtTime(args.time) && _srcClips[i] && _srcClips[i]->isConnected()) {
                    sources[i].reset(_srcClips[i]->fetchImage(args.time));
                }

                if (sources[i]) {
                    auto sb = sources[i]->getBounds();

                    if (sb.x2 > sb.x1 && sb.y2 > sb.y1) {
                        NatronVulkanImageHandle input = 0;

                        if (!sources[i]->getPixelData()) {
                            if (!hasVulkanClips()) {
                                OFX::throwSuiteStatusException(kOfxStatErrFormat);
                            }

                            NatronVulkanImageView view{};
                            check(renderSuite()->acquireImage(state.context, sources[i]->getPropertySet().propSetHandle(), &view));
                            sb = {view.bounds[0], view.bounds[1], view.bounds[2], view.bounds[3]};
                            input = view.image;
                        } else {
                            auto pixels = pack(*sources[i]);
                            nativeSources[i] = std::make_unique<ImageHandle>(state.imageSuite, state.context, sb.x2 - sb.x1, sb.y2 - sb.y1);
                            check(state.imageSuite->upload(state.context, nativeSources[i]->handle, pixels.data(), int64_t(sb.x2 - sb.x1) * 16));
                            input = nativeSources[i]->handle;
                        }

                        sourceBounds[i] = sb;
                        inputs[i] = {input, uint32_t(_inputWrap[i]->getValueAtTime(args.time)), uint32_t(_inputFilter[i]->getValueAtTime(args.time))};
                        floats(10 + i, {float(sb.x2 - sb.x1), float(sb.y2 - sb.y1), 1});
                    }
                } else floats(10 + i, {0, 0, 1});
                floats(14 + i, {time});
            }

            floats(18, {float(sourceBounds[0].x1), float(sourceBounds[0].y1)});

            for (unsigned i = 1; i < NBINPUTS; ++i) if (sources[i]) floats(18 + i, {float(sourceBounds[i].x1 - sourceBounds[0].x1), float(sourceBounds[i].y1 - sourceBounds[0].y1)});
            unsigned count = std::clamp(_paramCount->getValueAtTime(args.time), 0, int(_paramType.size()));

            for (const auto& uniform : shader.extra) for (unsigned i = 0; i < count; ++i) {
                    std::string name;
                    _paramName[i]->getValue(name);

                    if (name != uniform.name) {
                        continue;
                    }

                    int type = _paramType[i]->getValue();
                    double x = 0, y = 0, z = 0, w = 0;

                    if (uniform.type == "bool" && type == eUniformTypeBool) {
                        uniforms[uniform.slot][0] = _paramValueBool[i]->getValueAtTime(args.time);
                    } else if (uniform.type == "int" && type == eUniformTypeInt) {
                        int value = _paramValueInt[i]->getValueAtTime(args.time);
                        std::memcpy(uniforms[uniform.slot].data(), &value, sizeof(value));
                    } else {
                        if (uniform.type == "float" && type == eUniformTypeFloat) {
                            x = _paramValueFloat[i]->getValueAtTime(args.time);
                        } else if (uniform.type == "vec2" && type == eUniformTypeVec2) {
                            _paramValueVec2[i]->getValueAtTime(args.time, x, y);
                        } else if (uniform.type == "vec3" && type == eUniformTypeVec3) {
                            _paramValueVec3[i]->getValueAtTime(args.time, x, y, z);
                        } else if (uniform.type == "vec4" && type == eUniformTypeVec4) {
                            _paramValueVec4[i]->getValueAtTime(args.time, x, y, z, w);
                        }

                        floats(uniform.slot, {float(x), float(y), float(z), float(w)});
                    }

                    break;
                }

            std::unique_ptr<ImageHandle> output;

            if (!nativeClips) {
                output = std::make_unique<ImageHandle>(state.imageSuite, state.context, b.x2 - b.x1, b.y2 - b.y1);
            }

            const NatronVulkanImageHandle outputHandle = nativeClips ? nativeOutput.image : output->handle;
            NatronVulkanRegion roi{uint32_t(region.x1 - b.x1), uint32_t(region.y1 - b.y1), uint32_t(region.x2 - region.x1), uint32_t(region.y2 - region.y1)};
            check(state.shaderSuite->render(state.context, state.program, outputHandle, &roi, inputs.data(), uniforms.data(), shader.bytes));
            NatronVulkanDeviceInfo info{};
            check(state.imageSuite->getDeviceInfo(state.context, &info));

            if (info.validationErrors) {
                OFX::throwSuiteStatusException(kOfxStatFailed);
            }

            if (abort()) {
                return;
            }

            if (!nativeClips) {
                std::vector<float> pixels(size_t(b.x2 - b.x1) * (b.y2 - b.y1) * 4);
                check(state.imageSuite->download(state.context, outputHandle, pixels.data(), int64_t(b.x2 - b.x1) * 16));
                unpack(*destination, region, pixels);
            } else {
                ++clipRenders;
            }

            {
                AutoMutex informationLock(_rendererInfoMutex.get());
                _rendererInfo = std::string("Vulkan renderer\nDevice: ") + info.name + "\nAPI: " +
                                std::to_string(info.apiVersion >> 22) + "." + std::to_string((info.apiVersion >> 12) & 1023) +
                                "." + std::to_string(info.apiVersion & 4095);
            }

            {
                std::lock_guard<std::mutex> infoLock(infoMutex);
                lastInfo = info;
                ++nativeRenders;
            }

            clearPersistentMessage();
        }
        _imageShaderCompile->setEnabled(true);

        if (update) {
            _imageShaderParamsUpdated->setValue(!_imageShaderParamsUpdated->getValueAtTime(args.time));
        }
    } catch (const std::exception& error) {
        setPersistentMessage(OFX::Message::eMessageError, "", error.what());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
}
}
#ifdef _WIN32
#define NATRON_SHADER_EXPORT __declspec(dllexport)
#else
#define NATRON_SHADER_EXPORT __attribute__((visibility("default")))
#endif
extern "C" NATRON_SHADER_EXPORT uint64_t OfxShadertoyVulkanRenderCount()
{
    return Shadertoy::nativeRenders.load();
}
extern "C" NATRON_SHADER_EXPORT uint64_t OfxShadertoyVulkanClipRenderCount()
{
    return Shadertoy::clipRenders.load();
}
extern "C" NATRON_SHADER_EXPORT OfxStatus OfxShadertoyVulkanTransferCounts(uint64_t* uploads, uint64_t* downloads)
{
    if (!uploads || !downloads || !Shadertoy::renderSuite() || !Shadertoy::images()) {
        return kOfxStatErrValue;
    }

    NatronVulkanContextHandle context = 0;
    OfxStatus status = Shadertoy::images()->createContext(-1, 0, &context);

    if (status != kOfxStatOK) {
        return status;
    }

    status = Shadertoy::renderSuite()->getTransferCounts(context, uploads, downloads);
    Shadertoy::images()->releaseContext(context);
    return status;
}
extern "C" NATRON_SHADER_EXPORT int OfxShadertoyVulkanDeviceInfo(NatronVulkanDeviceInfo* info)
{
    if (!info) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(Shadertoy::infoMutex);
    *info = Shadertoy::lastInfo;
    return info->name[0] != 0;
}
