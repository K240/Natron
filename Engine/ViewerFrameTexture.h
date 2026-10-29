// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_VIEWERFRAMETEXTURE_H
#define NATRON_ENGINE_VIEWERFRAMETEXTURE_H
#include <Python.h>
#include "Global/Macros.h"
#include <memory>

NATRON_NAMESPACE_ENTER
// An upload receipt owned by the display backend. Engine scheduling never
// reads a graphics handle or assumes an OpenGL context is current.
class ViewerFrameTexture
{
public:
    virtual ~ViewerFrameTexture() = default;
};
using ViewerFrameTexturePtr = std::shared_ptr<ViewerFrameTexture>;
NATRON_NAMESPACE_EXIT
#endif
