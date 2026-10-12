// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_VULKAN_SHADER_SUITE_H
#define NATRON_OFX_VULKAN_SHADER_SUITE_H
#include "Global/OpenFX/NatronVulkanImageSuite.h"
#define kNatronOfxVulkanShaderSuite "NatronOfxVulkanShaderSuite"
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t NatronVulkanProgramHandle;
enum NatronVulkanTextureWrap {
    kNatronVulkanWrapRepeat = 0,
    kNatronVulkanWrapClamp = 1,
    kNatronVulkanWrapMirror = 2,
    kNatronVulkanWrapBlack = 3
};
typedef struct NatronVulkanShaderInput {
    NatronVulkanImageHandle image; // 0 samples transparent black.
    uint32_t wrap; // NatronVulkanTextureWrap
    uint32_t filter; // 0 nearest, 1 linear, 2 mipmap, 3 anisotropic.
} NatronVulkanShaderInput;
// Programs share a context with NatronOfxVulkanImageSuiteV1 images. Source is
// Vulkan GLSL with main(), RGBA output at location 0, sampler2D set 0 bindings
// 0..3, and a std140 uniform block at binding 4. No other descriptor bindings
// or stages are provided. Row order is unchanged. gl_FragCoord is in the full
// destination image pixel grid, including ROI renders. Derivatives are supported.
// uniformBytes is a positive multiple of 16, within the device's uniform limit.
// Program/image handles belong to exactly one context; releaseContext releases
// its programs too. Calls complete synchronously, serialize with image transfers,
// preserve destination pixels outside the ROI, and support aliased inputs/output.
typedef struct NatronOfxVulkanShaderSuiteV1 {
    // The optional UTF-8 error buffer is always NUL terminated when capacity > 0.
    // Compile failure returns kOfxStatErrValue and leaves program = 0.
    OfxStatus (*createProgram)(NatronVulkanContextHandle context, const char* source, uint32_t uniformBytes,
                               NatronVulkanProgramHandle* program, char *error, uint32_t errorCapacity);
    OfxStatus (*releaseProgram)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program);
    OfxStatus (*render)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program,
                        NatronVulkanImageHandle destination, const NatronVulkanRegion* region,
                        const NatronVulkanShaderInput inputs[4], const void *uniforms, uint32_t uniformBytes);
    // Active resource interface after compiler dead-code elimination. Sampler
    // bits 0..3 identify used inputs. Uniform offsets are bytes in binding 4.
    // Query with capacity 0/offsets NULL, then allocate uniformCount entries.
    // Insufficient nonzero capacity returns ErrValue with the required count.
    OfxStatus (*getResources)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program,
                              uint32_t *samplerMask, uint32_t *uniformCount, uint32_t *offsets, uint32_t capacity);
} NatronOfxVulkanShaderSuiteV1;
// V2 preserves V1 and adds immutable LUTs and explicit descriptor bindings.
// All descriptors use set 0. Input slots are sampler2D; LUTs may be 1D/2D/3D.
// Descriptor bindings must be unique. Data is copied synchronously at creation.
typedef struct NatronVulkanShaderLut {
    uint32_t binding;
    uint32_t dimensions; // 1, 2 or 3. 1D requires height=depth=1; 2D depth=1.
    uint32_t width, height, depth;
    uint32_t components; // 1, 3 or 4 float channels; x varies fastest.
    uint32_t filter; // 0 nearest or 1 linear; clamp to edge, no mipmaps.
    const float *values;
    uint64_t valueCount;
} NatronVulkanShaderLut;
typedef struct NatronVulkanShaderLayout {
    uint32_t uniformBinding;
    uint32_t inputCount; // 0..4, mapping the render() input slots to bindings.
    uint32_t inputBindings[4];
    uint32_t lutCount;
    const NatronVulkanShaderLut *luts;
} NatronVulkanShaderLayout;
typedef struct NatronOfxVulkanShaderSuiteV2 {
    OfxStatus (*createProgram)(NatronVulkanContextHandle context, const char* source, uint32_t uniformBytes,
                               const NatronVulkanShaderLayout* layout, NatronVulkanProgramHandle* program,
                               char *error, uint32_t errorCapacity);
    OfxStatus (*releaseProgram)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program);
    // Inputs beyond inputCount must have image=0. Other V1 render rules apply.
    OfxStatus (*render)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program,
                        NatronVulkanImageHandle destination, const NatronVulkanRegion* region,
                        const NatronVulkanShaderInput inputs[4], const void *uniforms, uint32_t uniformBytes);
    // Sampler mask bits identify the input slots, not their descriptor bindings.
    OfxStatus (*getResources)(NatronVulkanContextHandle context, NatronVulkanProgramHandle program,
                              uint32_t *samplerMask, uint32_t *uniformCount, uint32_t *offsets, uint32_t capacity);
} NatronOfxVulkanShaderSuiteV2;
#ifdef __cplusplus
}
#endif
#endif
