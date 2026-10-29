// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/VulkanImageStorage.h"
#include "Gpu/ImageOperations.h"
#include "Gpu/ViewerDisplayConversion.h"
#include "Gpu/FragmentProgram.h"
#include <stdexcept>
#include <limits>
NATRON_NAMESPACE_ENTER
struct VulkanImageContext::Impl {
    std::shared_ptr<Gpu::VulkanDevice> device;
    Gpu::ImageOperations operations;
    std::mutex mutex;
    uint64_t uploads = 0, downloads = 0;
    explicit Impl(std::shared_ptr<Gpu::VulkanDevice> value) : device(std::move(value)), operations(device) {}
};
VulkanImageContext::VulkanImageContext(std::shared_ptr<Gpu::VulkanDevice> device) : _imp(new Impl(std::move(device))) {}
VulkanImageContext::~VulkanImageContext() = default;
const std::shared_ptr<Gpu::VulkanDevice> &VulkanImageContext::device() const
{
    return _imp->device;
}
std::array<uint64_t, 2> VulkanImageContext::transferCounts() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    return {_imp->uploads, _imp->downloads};
}
struct VulkanImageStorage::Impl {
    std::shared_ptr<VulkanImageContext> context;
    Gpu::VulkanImage image;
    Impl(std::shared_ptr<VulkanImageContext> value, uint32_t width, uint32_t height)
        : context(std::move(value)), image(context ? context->device() : nullptr, width, height) {}
};
VulkanImageStorage::VulkanImageStorage(std::shared_ptr<VulkanImageContext> context, uint32_t width, uint32_t height)
    : _imp(new Impl(std::move(context), width, height))
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->image.clear({0, 0, 0, 0});
}
VulkanImageStorage::~VulkanImageStorage() = default;
struct VulkanFragmentProgram::Impl {
    std::shared_ptr<VulkanImageContext> context;
    Gpu::FragmentProgram program;
    static Gpu::FragmentLayout nativeLayout(const NatronVulkanShaderLayout *layout)
    {
        Gpu::FragmentLayout result;

        if (!layout) {
            return result;
        }

        if (layout->inputCount > 4 || layout->lutCount > 256 || (layout->lutCount && !layout->luts)) {
            throw std::invalid_argument("Invalid fragment layout array.");
        }

        result.uniformBinding = layout->uniformBinding;
        result.inputBindings.assign(layout->inputBindings, layout->inputBindings + layout->inputCount);

        for (uint32_t i = 0; i < layout->lutCount; ++i) {
            const auto &lut = layout->luts[i];

            if (lut.valueCount > std::numeric_limits<size_t>::max()) {
                throw std::invalid_argument("LUT value count overflow.");
            }

            result.luts.push_back({lut.binding, lut.dimensions, lut.width, lut.height, lut.depth, lut.components,
                                   lut.filter, lut.values, static_cast<size_t>(lut.valueCount)});
        }

        return result;
    }
    Impl(std::shared_ptr<VulkanImageContext> value, const std::string& source, uint32_t bytes, const NatronVulkanShaderLayout *layout)
        : context(std::move(value)), program(context ? context->device() : nullptr, source, bytes, nativeLayout(layout)) {}
};
VulkanFragmentProgram::VulkanFragmentProgram(std::shared_ptr<VulkanImageContext> context, const std::string& source, uint32_t bytes)
    : VulkanFragmentProgram(std::move(context), source, bytes, nullptr) {}
VulkanFragmentProgram::VulkanFragmentProgram(std::shared_ptr<VulkanImageContext> context, const std::string& source, uint32_t bytes,
        const NatronVulkanShaderLayout *layout)
    : _imp(new Impl(std::move(context), source, bytes, layout)) {}
VulkanFragmentProgram::~VulkanFragmentProgram() = default;
const std::vector<uint32_t> &VulkanFragmentProgram::activeUniformOffsets() const
{
    return _imp->program.activeUniformOffsets();
}
uint32_t VulkanFragmentProgram::activeSamplers() const
{
    return _imp->program.activeSamplers();
}
void VulkanFragmentProgram::render(VulkanImageStorage& destination, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   const std::array<VulkanFragmentInput, 4> &inputs, const void *uniforms, uint32_t bytes)
{
    if (destination.context() != _imp->context) {
        throw std::invalid_argument("Fragment destination belongs to a different context.");
    }

    std::array<Gpu::FragmentInput, 4> nativeInputs{};

    for (unsigned i = 0; i < inputs.size(); ++i) {
        if (inputs[i].image && inputs[i].image->context() != _imp->context) {
            throw std::invalid_argument("Fragment input belongs to a different context.");
        }

        nativeInputs[i] = {inputs[i].image ? &inputs[i].image->_imp->image : nullptr,
                           static_cast<Gpu::TextureWrap>(inputs[i].wrap), inputs[i].filter
                          };
    }

    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->program.render(destination._imp->image, {x, y, width, height}, nativeInputs, uniforms, bytes);
}
size_t VulkanImageStorage::byteSize() const
{
    return _imp->image.byteSize();
}
uint32_t VulkanImageStorage::width() const
{
    return _imp->image.width();
}
uint32_t VulkanImageStorage::height() const
{
    return _imp->image.height();
}
const std::shared_ptr<VulkanImageContext> &VulkanImageStorage::context() const
{
    return _imp->context;
}
void VulkanImageStorage::checkContext(const VulkanImageStorage* other) const
{
    if (other && other->_imp->context != _imp->context) {
        throw std::invalid_argument("Vulkan images belong to different contexts.");
    }
}
void VulkanImageStorage::upload(const void* bytes, size_t count)
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->image.upload(bytes, count);
    ++_imp->context->_imp->uploads;
}
void VulkanImageStorage::uploadRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* bytes, size_t count)
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->image.uploadRegion(x, y, width, height, bytes, count);
    ++_imp->context->_imp->uploads;
}
std::vector<unsigned char> VulkanImageStorage::download() const
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    auto bytes = _imp->image.download();
    ++_imp->context->_imp->downloads;
    return bytes;
}
void VulkanImageStorage::uploadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
        const void *bytes, size_t count, size_t rowBytes)
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->image.uploadRegionRows(x, y, width, height, bytes, count, rowBytes);
    ++_imp->context->_imp->uploads;
}
void VulkanImageStorage::downloadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
        void *bytes, size_t count, size_t rowBytes) const
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->image.downloadRegionRows(x, y, width, height, bytes, count, rowBytes);
    ++_imp->context->_imp->downloads;
}
void VulkanImageStorage::copy(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t dx, uint32_t dy)
{
    checkContext(&source);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.copy(source._imp->image, _imp->image, {x, y, width, height}, dx, dy);
}
void VulkanImageStorage::copyTo(Gpu::VulkanImage& destination, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t dx, uint32_t dy, bool forceOpaque) const
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.copy(_imp->image, destination, {x, y, width, height}, dx, dy);

    if (forceOpaque) {
        _imp->context->_imp->operations.forceOpaque(destination, {dx, dy, width, height});
    }
}
void VulkanImageStorage::displayTo(Gpu::ViewerDisplayConversion& conversion, Gpu::VulkanImage& destination,
                                   uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   const Gpu::ViewerDisplayCorrection& correction, const Gpu::ViewerDisplayTables& tables) const
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    conversion.render(_imp->image, destination, {x, y, width, height}, correction, tables);
}
void VulkanImageStorage::fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const std::array<float, 4> &color)
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.fill(_imp->image, {x, y, width, height}, color);
}
void VulkanImageStorage::halve(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               int32_t sourceOffsetX, int32_t sourceOffsetY)
{
    checkContext(&source);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.halve(source._imp->image, _imp->image, {x, y, width, height}, sourceOffsetX, sourceOffsetY);
}
void VulkanImageStorage::upscale(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                 int32_t sourceOffsetX, int32_t sourceOffsetY, uint32_t levels)
{
    checkContext(&source);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.upscale(source._imp->image, _imp->image, {x, y, width, height}, sourceOffsetX, sourceOffsetY, levels);
}
void VulkanImageStorage::transform(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   const std::array<float, 4> &gain, const std::array<float, 4> &offset)
{
    checkContext(&source);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.transform(source._imp->image, _imp->image, {x, y, width, height}, gain, offset);
}
void VulkanImageStorage::mix(const VulkanImageStorage& processed, const VulkanImageStorage* original, const VulkanImageStorage* mask,
                             uint32_t x, uint32_t y, uint32_t width, uint32_t height, float amount, bool masked, bool inverted)
{
    checkContext(&processed);
    checkContext(original);
    checkContext(mask);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.mix(processed._imp->image, original ? &original->_imp->image : nullptr,
                                        mask ? &mask->_imp->image : nullptr, _imp->image, {x, y, width, height}, amount, masked, inverted);
}
void VulkanImageStorage::restore(const VulkanImageStorage& processed, const VulkanImageStorage* original,
                                 uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t channels)
{
    checkContext(&processed);
    checkContext(original);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.restore(processed._imp->image, original ? &original->_imp->image : nullptr,
                                            _imp->image, {x, y, width, height}, channels);
}
void VulkanImageStorage::convertColor(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                      uint32_t sourceSpace, uint32_t destinationSpace)
{
    checkContext(&source);
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    _imp->context->_imp->operations.convertColor(source._imp->image, _imp->image, {x, y, width, height}, sourceSpace, destinationSpace);
}
bool VulkanImageStorage::checkForNaNs(uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool fix)
{
    std::lock_guard<std::mutex> lock(_imp->context->_imp->mutex);
    return _imp->context->_imp->operations.checkForNaNs(_imp->image, {x, y, width, height}, fix);
}
NATRON_NAMESPACE_EXIT
