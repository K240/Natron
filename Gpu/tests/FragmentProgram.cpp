// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/FragmentProgram.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace Natron::Gpu;
namespace
{
constexpr unsigned W = 37, H = 23, SW = 17, SH = 11;
const ImageRegion roi{3, 2, 29, 17};
const char *sampleShader = R"(#version 450
layout(set=0,binding=0) uniform sampler2D channel0;
layout(set=0,binding=1) uniform sampler2D channel1;
layout(set=0,binding=2) uniform sampler2D channel2;
layout(set=0,binding=3) uniform sampler2D channel3;
layout(set=0,binding=4,std140) uniform Params { vec4 gain; vec4 offset; vec4 scale; };
layout(location=0) out vec4 color;
void main() {
 vec2 uv=gl_FragCoord.xy*scale.xy+scale.zw;
 color=(texture(channel0,uv)+texture(channel1,uv)*0.5+texture(channel2,uv)*0.25+texture(channel3,uv)*0.125)*gain+offset;
})";
void require(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
template<class F> void rejects(F&& operation)
{
    bool rejected = false;

    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Invalid fragment operation was accepted.");
}
bool inside(unsigned x, unsigned y)
{
    return x >= roi.x && x < roi.x + roi.width && y >= roi.y && y < roi.y + roi.height;
}
int index(int value, int count, TextureWrap wrap)
{
    if (wrap == TextureWrap::Black && (value < 0 || value >= count)) {
        return -1;
    }

    if (wrap == TextureWrap::Clamp) {
        return std::clamp(value, 0, count - 1);
    }

    if (wrap == TextureWrap::Mirror) {
        int n = ((value % (2 * count)) + 2 * count) % (2 * count);
        return n < count ? n : 2 * count - 1 - n;
    }

    return ((value % count) + count) % count;
}
float sample(const std::vector<float> &values, float u, float v, unsigned c, TextureWrap wrap, bool linear)
{
    auto pixel = [&](int x, int y) {
        x = index(x, SW, wrap);
        y = index(y, SH, wrap);
        return x < 0 || y < 0 ? 0.f : values[(y * SW + x) * 4 + c];
    };

    if (!linear) {
        return pixel(static_cast<int>(std::floor(u * SW)), static_cast<int>(std::floor(v * SH)));
    }

    float x = u * SW - .5f, y = v * SH - .5f;
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float a = x - ix, b = y - iy;
    return (pixel(ix, iy) * (1 - a) + pixel(ix + 1, iy) * a) * (1 - b) +
           (pixel(ix, iy + 1) * (1 - a) + pixel(ix + 1, iy + 1) * a) * b;
}
float compare(VulkanImage& output, const std::vector<float> &expected)
{
    auto bytes = output.download();
    float maximum = 0;

    for (size_t i = 0; i < expected.size(); ++i) {
        float value;
        std::memcpy(&value, bytes.data() + i * 4, 4);
        float error = std::abs(value - expected[i]);

        if (!std::isfinite(value) || error > 0.00003f) {
            throw std::runtime_error("Fragment pixel " + std::to_string(i) + ": " + std::to_string(value) + " expected " + std::to_string(expected[i]));
        }

        maximum = std::max(maximum, error);
    }

    return maximum;
}
}
int main(int argc, char** argv)
{
    try {
        int selected = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            selected = std::stoi(argv[2]);
        }

        auto instance = VulkanInstance::create({true, {}});
        unsigned cases = 0;
        float maximum = 0;
        std::string name;
        {
            auto device = VulkanDevice::create(instance, selected);
            name = device->properties().deviceName;
            FragmentProgram program(device, sampleShader, 48);
            require(program.activeSamplers() == 15 && program.activeUniformOffsets() == std::vector<uint32_t>({0, 16, 32}),
                    "Compiled shader resource interface differs from its live bindings.");
            std::array<std::unique_ptr<VulkanImage>, 4> sources;
            std::array<std::vector<float>, 4> data;

            for (unsigned i = 0; i < 4; ++i) {
                sources[i] = std::make_unique<VulkanImage>(device, SW, SH);
                data[i].resize(SW * SH * 4);

                for (size_t j = 0; j < data[i].size(); ++j) {
                    data[i][j] = static_cast<float>((j * 17 + i * 19) % 79) / 53.f - .5f;
                }

                sources[i]->upload(data[i].data(), data[i].size() * 4);
            }

            // Eighth-texel positions are exact on both devices, avoiding vendor
            // interpolation-weight quantization in this arithmetic comparison.
            const std::array<float, 12> uniforms{1.3f, -.4f, .7f, 1.f, .1f, -.3f, .5f, .2f,
                                                 5.f / (4 * SW), 5.f / (4 * SH), -9.f / (4 * SW), -7.f / (4 * SH)};

            for (unsigned wrap = 0; wrap < 4; ++wrap) for (unsigned linear = 0; linear < 2; ++linear) for (unsigned count : {
                            0u, 1u, 4u
                        }) {
                        VulkanImage output(device, W, H);
                        output.clear({-2, -2, -2, -2});
                        std::array<FragmentInput, 4> inputs{};

                        for (unsigned i = 0; i < 4; ++i) inputs[i] = {i < count ? sources[i].get() : nullptr, static_cast<TextureWrap>(wrap), linear != 0};

                        program.render(output, roi, inputs, uniforms.data(), sizeof(uniforms));

                        std::vector<float> expected(W * H * 4, -2.f);

                        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) if (inside(x, y)) {
                                    float u = (x + .5f) * uniforms[8] + uniforms[10], v = (y + .5f) * uniforms[9] + uniforms[11];

                                    for (unsigned c = 0; c < 4; ++c) {
                                        float sum = 0, weight = 1;

                                        for (unsigned i = 0; i < count; ++i, weight *= .5f) {
                                            sum += sample(data[i], u, v, c, static_cast<TextureWrap>(wrap), linear != 0) * weight;
                                        }

                                        expected[(y * W + x) * 4 + c] = sum * uniforms[c] + uniforms[4 + c];
                                    }
                                }

                        maximum = std::max(maximum, compare(output, expected));
                        ++cases;
                    }
            FragmentProgram mipProgram(device, R"(#version 450
layout(set=0,binding=0) uniform sampler2D input0;
layout(set=0,binding=4,std140) uniform Params { ivec4 values; };
layout(location=0) out vec4 color;
void main(){ivec2 p=ivec2(gl_FragCoord.xy)%values.y;color=textureLod(input0,(vec2(p)+.5)/float(values.y),float(values.x));})", 16);
            VulkanImage mipSource(device, 16, 16);
            std::vector<float> mipData(16 * 16 * 4);

            for (size_t i = 0; i < mipData.size(); ++i) {
                mipData[i] = float((i * 19) % 97) / 31.f - .7f;
            }

            mipSource.upload(mipData.data(), mipData.size() * 4);

            for (unsigned filter : {
                     2u, 3u

                 }) for (unsigned level = 0; level < 5; ++level) {
                    VulkanImage mipOutput(device, W, H);
                    std::array<int, 4> params{int(level), 16 >> level, 0, 0};
                    std::array<FragmentInput, 4> mipInputs{};
                    mipInputs[0] = {&mipSource, TextureWrap::Clamp, filter};
                    mipProgram.render(mipOutput, roi, mipInputs, params.data(), sizeof(params));
                    std::vector<float> reference(W * H * 4);
                    unsigned tile = 1u << level, size = 16 >> level;

                    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) if (inside(x, y)) for (unsigned c = 0; c < 4; ++c) {
                                    float sum = 0;

                                    for (unsigned sy = 0; sy < tile; ++sy) for (unsigned sx = 0; sx < tile; ++sx) {
                                            sum += mipData[(((y % size) * tile + sy) * 16 + (x % size) * tile + sx) * 4 + c];
                                        }

                                    reference[(y * W + x) * 4 + c] = sum / (tile * tile);
                                }

                    maximum = std::max(maximum, compare(mipOutput, reference));
                    ++cases;
                }

            FragmentProgram derivatives(device, R"(#version 450
layout(location=0) out vec4 color;
void main(){color=vec4(dFdx(gl_FragCoord.x),dFdy(gl_FragCoord.y),fwidth(2.0*gl_FragCoord.x+3.0*gl_FragCoord.y),gl_FragCoord.z*2.0*gl_FragCoord.w);})", 16);
            require(derivatives.activeSamplers() == 0 && derivatives.activeUniformOffsets().empty(),
                    "Unused shader resources were reported as active.");
            VulkanImage output(device, W, H);
            std::array<float, 4> unused{};
            derivatives.render(output, roi, {}, unused.data(), sizeof(unused));
            std::vector<float> expected(W * H * 4);

            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) if (inside(x, y)) {
                        const std::array<float, 4> value{1, 1, 5, 1};
                        std::copy(value.begin(), value.end(), expected.begin() + (y * W + x) * 4);
                    }

            maximum = std::max(maximum, compare(output, expected));
            ++cases;
            FragmentProgram alias(device, R"(#version 450
layout(set=0,binding=0) uniform sampler2D input0;
layout(location=0) out vec4 color;
void main(){color=texelFetch(input0,ivec2(gl_FragCoord.xy),0)*2.0+vec4(.1);})", 16);
            std::vector<float> initial(W * H * 4, .3f);
            output.upload(initial.data(), initial.size() * 4);
            std::array<FragmentInput, 4> inputs{};
            inputs[0].image = &output;
            alias.render(output, roi, inputs, unused.data(), sizeof(unused));

            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) for (unsigned c = 0; c < 4; ++c) {
                        initial[(y * W + x) * 4 + c] = inside(x, y) ? .7f : .3f;
                    }

            maximum = std::max(maximum, compare(output, initial));
            ++cases;
            std::vector<std::future<float>> workers;

            for (unsigned i = 0; i < 4; ++i) workers.emplace_back(std::async(std::launch::async, [&] {
                VulkanImage out(device, W, H);
                derivatives.render(out, roi, {}, unused.data(), sizeof(unused));
                return compare(out, expected);
            }));

            for (auto& worker : workers) {
                maximum = std::max(maximum, worker.get());
                ++cases;
            }

            rejects([&] { FragmentProgram invalid(device, "#version 450\nvoid main(){broken syntax}", 16); });
            rejects([&] { FragmentProgram invalid(device, sampleShader, 17); });
            rejects([&] { program.render(output, roi, {}, nullptr, 48); });
            rejects([&] { program.render(output, {UINT32_MAX, 0, 1, 1}, {}, uniforms.data(), 48); });
            VulkanImage uninitialized(device, SW, SH);
            inputs[0].image = &uninitialized;
            rejects([&] { program.render(output, roi, inputs, uniforms.data(), 48); });
            auto foreignDevice = VulkanDevice::create(instance, selected);
            VulkanImage foreign(foreignDevice, W, H);
            rejects([&] { program.render(foreign, roi, {}, uniforms.data(), 48); });
            inputs[0].image = nullptr;
            inputs[0].wrap = static_cast<TextureWrap>(9);
            rejects([&] { program.render(output, roi, inputs, uniforms.data(), 48); });
            device->waitIdle();
        }
        require(instance->validationErrorCount() == 0, "Fragment Vulkan validation failed.");
#ifdef _WIN32
        require(!GetModuleHandleW(L"opengl32.dll"), "Fragment renderer loaded OpenGL.");
#endif
        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"referenceCases\":" << cases
                  << ",\"invalidCases\":7,\"hostWorkers\":4,\"maxAbsoluteError\":" << maximum
                  << ",\"validationErrors\":0,\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
