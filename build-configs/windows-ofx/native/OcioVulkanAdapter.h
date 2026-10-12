// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_OCIOVULKANADAPTER_H
#define NATRON_OFX_OCIOVULKANADAPTER_H
#include <OpenColorIO/OpenColorIO.h>
#include "ofxsImageEffect.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
bool nativeOCIOAvailable();
bool nativeOCIOClipsAvailable();
void describeNativeIO(OFX::ImageEffectDescriptor& descriptor, OFX::PageParamDescriptor* page);
void updateNativeIOSupport(OFX::ImageEffect& instance);
bool uploadNativeReader(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                        const float *pixels, const OfxRectI& bounds, int64_t rowBytes, const OfxRectI& crop,
                        OFX::Image& destination, const OfxRectI& window, unsigned levels, bool unpremult, bool premult);
void clearNativeIO(OFX::ImageEffect& instance, OFX::Image& destination, const OfxRectI& window);
bool downloadNativeWriter(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                          const OFX::Image& source, const OfxRectI& window, float *pixels, int64_t rowBytes,
                          bool opaque, bool unpremult, bool premult);
void copyNativeIO(OFX::ImageEffect& instance, const OFX::Image& source, OFX::Image& destination, const OfxRectI& window);
void setNativeOCIOClipSupport(OFX::ImageEffect& instance, bool enabled);
bool renderNativeOCIOClips(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                           const OFX::Image& source, OFX::Image& destination, const OfxRectI& window,
                           bool premult, float mix, bool masked, bool inverted, const OFX::Image* mask);
bool applyNativeOCIO(OFX::ImageEffect &instance, const OCIO_NAMESPACE::ConstProcessorRcPtr &processor,
                     float *pixels, uint32_t width, uint32_t height, uint32_t components, int64_t rowBytes);
void releaseNativeOCIO(OFX::ImageEffect *instance);
#endif
