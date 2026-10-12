// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/FragmentProgram.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace Natron::Gpu;
namespace
{
constexpr unsigned W = 31, H = 19;
const ImageRegion roi{2, 3, 25, 13};
void require(bool value, const char *message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
template<class F> void reject(F &&operation)
{
    bool rejected = false;

    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Invalid LUT program was accepted.");
}
float compare(VulkanImage &image, const std::vector<float> &reference)
{
    auto bytes = image.download();
    require(bytes.size() == reference.size() * sizeof(float), "LUT output size differs.");
    float maximum = 0;

    for (size_t i = 0; i < reference.size(); ++i) {
        float actual;
        std::memcpy(&actual, bytes.data() + i * sizeof(float), sizeof(float));
        require(std::isfinite(actual), "Non-finite LUT output.");
        maximum = std::max(maximum, std::abs(actual - reference[i]));
    }

    require(maximum <= .00003f, "LUT pixel reference differs.");
    return maximum;
}
float sample(const std::vector<float> &data, unsigned components, unsigned w, unsigned h, unsigned d,
             float x, float y, float z, unsigned channel, bool linear)
{
    if (channel >= components) {
        return channel == 3 ? 1.f : 0.f;
    }

    auto value = [&](int ix, int iy, int iz) {
        ix = std::clamp(ix, 0, int(w) - 1);
        iy = std::clamp(iy, 0, int(h) - 1);
        iz = std::clamp(iz, 0, int(d) - 1);
        return data[((iz * h + iy) * w + ix) * components + channel];
    };

    if (!linear) {
        return value(int(std::floor(x * w)), int(std::floor(y * h)), int(std::floor(z * d)));
    }

    x = x * w - .5f;
    y = y * h - .5f;
    z = z * d - .5f;
    int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    float dx = x - ix, dy = y - iy, dz = z - iz, result = 0;

    for (int k = 0; k < 2; ++k) for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                result += value(ix + i, iy + j, iz + k) * (i ? dx : 1 - dx) * (j ? dy : 1 - dy) * (k ? dz : 1 - dz);
            }

    return result;
}
}
int main(int argc, char **argv)
{
    try {
        int selected = argc == 3 && std::string(argv[1]) == "--gpu" ? std::stoi(argv[2]) : -1;
        auto instance = VulkanInstance::create({true, {}});
        unsigned cases = 0, invalid = 0;
        float maximum = 0;
        std::string name;
        {
            auto device = VulkanDevice::create(instance, selected);
            name = device->properties().deviceName;
            VulkanImage source(device, W, H), output(device, W, H);
            std::vector<float> input(W * H * 4);

            for (size_t i = 0; i < input.size(); ++i) {
                input[i] = float(i % 17) / 16.f;
            }

            source.upload(input.data(), input.size() * sizeof(float));

            for (unsigned dim : {
                     1u, 2u, 3u

                 }) for (unsigned channels : {
                                 1u, 3u, 4u

                             }) for (unsigned filter : {
                                             0u, 1u
                                         }) {
                        const unsigned w = 8, h = dim >= 2 ? 4 : 1, d = dim == 3 ? 2 : 1;
                        std::vector<float> values(w * h * d * channels);

                        for (unsigned z = 0; z < d; ++z) for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (unsigned c = 0; c < channels; ++c) {
                                        values[((z * h + y) * w + x) * channels + c] = x * .25f + y * .5f + z * 1.5f + c * .125f - .75f;
                                    }

                        FragmentLayout layout;
                        layout.uniformBinding = 17;
                        layout.inputBindings = {7};
                        layout.luts = {{23, dim, w, h, d, channels, filter, values.data(), values.size()}};
                        std::string shader = "#version 450\nlayout(set=0,binding=23) uniform sampler" + std::to_string(dim) + "D lut;\n"
                                             "layout(set=0,binding=7) uniform sampler2D image;\n"
                                             "layout(set=0,binding=17,std140) uniform Params {vec4 gain;};\nlayout(location=0) out vec4 color;\n"
                                             "void main(){ivec2 p=ivec2(gl_FragCoord.xy);vec3 uv=vec3((float(p.x%12)-2.0+.625)/8.0,"
                                             "(float(p.y%8)-2.0+.375)/4.0,(float((p.x+p.y)%4)-1.0+.875)/2.0);"
                                             "color=texture(lut," + (dim == 1 ? std::string("uv.x") : dim == 2 ? std::string("uv.xy") : std::string("uv")) +
                                             ")*gain+texelFetch(image,p,0)*.125;}";
                        FragmentProgram program(device, shader, 16, layout);
                        require(program.activeSamplers() == 1 && program.activeUniformOffsets() == std::vector<uint32_t>({0}), "LUT layout reflection differs.");
                        auto saved = values;
                        std::fill(values.begin(), values.end(), 999.f); // The program owns its original LUT.
                        const std::array<float, 4> gain{1.125f, .5f, 1.5f, .75f};
                        std::array<FragmentInput, 4> inputs{};
                        inputs[0].image = &source;
                        output.clear({-.25f, -.25f, -.25f, -.25f});
                        program.render(output, roi, inputs, gain.data(), sizeof(gain));
                        std::vector<float> expected(W * H * 4, -.25f);

                        for (unsigned y = roi.y; y < roi.y + roi.height; ++y) for (unsigned x = roi.x; x < roi.x + roi.width; ++x) for (unsigned c = 0; c < 4; ++c) {
                                    float u = (float(x % 12) - 2.f + .625f) / 8.f;
                                    float v = dim >= 2 ? (float(y % 8) - 2.f + .375f) / 4.f : .5f;
                                    float t = dim == 3 ? (float((x + y) % 4) - 1.f + .875f) / 2.f : .5f;
                                    size_t i = (y * W + x) * 4 + c;
                                    expected[i] = sample(saved, channels, w, h, d, u, v, t, c, filter) * gain[c] + input[i] * .125f;
                                }

                        maximum = std::max(maximum, compare(output, expected));
                        ++cases;

                        if (dim == 3 && channels == 3 && filter == 1) {
                            source.upload(input.data(), input.size() * sizeof(float));
                            program.render(source, roi, inputs, gain.data(), sizeof(gain));

                            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) if (x < roi.x || x >= roi.x + roi.width || y < roi.y || y >= roi.y + roi.height)
                                        for (unsigned c = 0; c < 4; ++c) {
                                            expected[(y * W + x) * 4 + c] = input[(y * W + x) * 4 + c];
                                        }

                            maximum = std::max(maximum, compare(source, expected));
                            source.upload(input.data(), input.size() * sizeof(float));
                            ++cases;
                        }
                    }
            const char *code = "#version 450\nlayout(set=0,binding=23) uniform sampler3D lut;layout(location=0) out vec4 color;void main(){color=texture(lut,vec3(.5));}";
            std::vector<float> values(24, .5f);
            FragmentLayout layout;
            layout.uniformBinding = 0;
            layout.inputBindings = {};
            FragmentLut good{23, 3, 2, 2, 2, 3, 1, values.data(), values.size()};

            for (unsigned kind = 0; kind < 9; ++kind) {
                layout.luts = {good};

                if (kind == 0) {
                    layout.luts[0].valueCount--;
                }

                if (kind == 1) {
                    layout.luts[0].dimensions = 1;
                }

                if (kind == 2) {
                    layout.luts[0].filter = 2;
                }

                if (kind == 3) {
                    layout.luts[0].values = nullptr;
                }

                if (kind == 4) {
                    layout.luts[0].components = 2;
                }

                if (kind == 5) {
                    layout.luts.push_back(good);
                }

                if (kind == 6) {
                    layout.luts[0].binding = 0;
                }

                if (kind == 7) {
                    layout.luts[0].dimensions = 2;
                    layout.luts[0].depth = 1;
                    layout.luts[0].valueCount = 12;
                }

                if (kind == 8) {
                    layout.luts[0].binding = 19;
                }

                reject([&] {FragmentProgram bad(device, code, 16, layout);});
                ++invalid;
            }

            device->waitIdle();
        }
        require(instance->validationErrorCount() == 0, "LUT Vulkan validation failed.");
#ifdef _WIN32
        require(!GetModuleHandleW(L"opengl32.dll"), "LUT renderer loaded OpenGL.");
#endif
        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"referenceCases\":" << cases << ",\"invalidCases\":" << invalid
                  << ",\"maxAbsoluteError\":" << maximum << ",\"validationErrors\":0,\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
