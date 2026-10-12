// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_LUTTEXTURE_H
#define NATRON_GPU_LUTTEXTURE_H
#include "Gpu/FragmentProgram.h"
namespace Natron
{
namespace Gpu
{
class LutTexture
{
public:
    LutTexture(std::shared_ptr<VulkanDevice> device, const FragmentLut &data);
    ~LutTexture();
    LutTexture(const LutTexture &) = delete;
    LutTexture &operator=(const LutTexture &) = delete;
    VkImageView view() const
    {
        return _view;
    }
    uint32_t binding() const
    {
        return _binding;
    }
    uint32_t filter() const
    {
        return _filter;
    }
private:
    std::shared_ptr<VulkanDevice> _device;
    VkImage _image = VK_NULL_HANDLE;
    VkImageView _view = VK_NULL_HANDLE;
    VmaAllocation_T *_allocation = nullptr;
    uint32_t _binding, _filter;
};
}
}
#endif
