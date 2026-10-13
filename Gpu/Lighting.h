// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_LIGHTING_H
#define NATRON_GPU_LIGHTING_H
#include "Gpu/CardScene.h"
#include <cstdint>
namespace Natron { namespace Gpu { namespace Cards {
constexpr size_t maxLights = 16;
struct alignas(16) GpuMaterial {float base[4], emission[4], surface[4];};
struct alignas(16) GpuLight {float position[4], direction[4], color[4], cone[4], environmentInverse[16];uint32_t environment[4];};
void validateMaterial(const Material& material);
void validateLights(const std::vector<Light>& lights);
void packMaterial(GpuMaterial& output,const Material& material);
void packLights(GpuLight* output,const std::vector<Light>& lights);
Vec3 normalVector(const Matrix& inverse,Vec3 normal);
Vec3 shadeDirect(const Material& material,Vec3 base,Vec3 normal,Vec3 view,Vec3 towardLight,Vec3 radiance);
Matrix environmentToLocal(const Matrix& transform);
Vec3 environmentRadiance(const Light& light,const Matrix& inverse,Vec3 direction);
struct EnvironmentSample {Vec3 direction, weight;};
EnvironmentSample environmentSample(const Material& material,Vec3 base,Vec3 normal,Vec3 view,
                                    unsigned index,unsigned count,bool specular);
std::string lightingShader();
}}}
#endif
