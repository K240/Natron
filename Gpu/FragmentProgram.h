// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_FRAGMENTPROGRAM_H
#define NATRON_GPU_FRAGMENTPROGRAM_H
#include "Gpu/ImageOperations.h"
namespace Natron
{
namespace Gpu
{
enum class TextureWrap { Repeat, Clamp, Mirror, Black };
struct FragmentInput {
    VulkanImage *image = nullptr;
    TextureWrap wrap = TextureWrap::Clamp;
    uint32_t filter = 0; // nearest, linear, mipmap, anisotropic
};
// Immutable float LUT data is copied at construction; x varies fastest.
struct FragmentLut {
    uint32_t binding = 0;
    uint32_t dimensions = 2;
    uint32_t width = 0, height = 1, depth = 1;
    uint32_t components = 3; // 1, 3 or 4; RGB is uploaded as RGBA.
    uint32_t filter = 1; // nearest or linear, clamp to edge.
    const float *values = nullptr;
    size_t valueCount = 0;
};
struct FragmentLayout {
    uint32_t uniformBinding = 4;
    std::vector<uint32_t> inputBindings{0, 1, 2, 3};
    std::vector<FragmentLut> luts;
};
// Default Vulkan GLSL: set 0 bindings 0..3 are sampler2D; binding 4 is a std140 UBO.
// Output location 0 is RGBA32F. gl_FragCoord has the image's unchanged row order.
// All transfers complete before returning. Callers serialize image access.
class FragmentProgram
{
public:
    FragmentProgram(std::shared_ptr<VulkanDevice> device, const std::string& source, uint32_t uniformBytes);
    FragmentProgram(std::shared_ptr<VulkanDevice> device, const std::string& source, uint32_t uniformBytes,
                    const FragmentLayout &layout);
    ~FragmentProgram();
    const std::vector<uint32_t> &activeUniformOffsets() const;
    uint32_t activeSamplers() const;
    FragmentProgram(const FragmentProgram &) = delete;
    FragmentProgram &operator=(const FragmentProgram &) = delete;
    void render(VulkanImage& destination, ImageRegion region, const std::array<FragmentInput, 4> &inputs,
                const void *uniforms, size_t uniformBytes);
private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
};
}
}
#endif
