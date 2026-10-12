// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_OFXDRAWSUITE_H
#define NATRON_ENGINE_OFXDRAWSUITE_H
#include <Python.h>
#include "Global/Macros.h"
#include "Global/OpenFX/OverlayInteractV2.h"
#include "Global/OpenFX/NatronDrawSuite.h"
#include "Engine/OverlayDrawList.h"
#include <array>

NATRON_NAMESPACE_ENTER

const OfxDrawSuiteV1 &ofxDrawSuite();
const NatronOfxDrawSuiteV1 &ofxNatronDrawSuite();

// A handle is valid only on this thread and until this draw action returns.
// Opaque, monotonically assigned tokens prevent a later draw from reviving a
// stale handle; suite callbacks never dereference an unrecognized pointer.
class OfxDrawSession
{
public:
    using Colors = std::array<OfxRGBAColourF, 7>;
    static Colors defaultColors();
    OfxDrawSession(OverlayDrawList& commands, const Colors& colors,
                   double pixelScaleX, double pixelScaleY, double devicePixelRatio);
    ~OfxDrawSession();
    OfxDrawSession(const OfxDrawSession &) = delete;
    OfxDrawSession &operator=(const OfxDrawSession &) = delete;

    OfxDrawContextHandle handle() const
    {
        return _handle;
    }
    OverlayDrawList &commands() const
    {
        return _commands;
    }
    const Colors &colors() const
    {
        return _colors;
    }
    double pixelScaleX() const
    {
        return _pixelScaleX;
    }
    double pixelScaleY() const
    {
        return _pixelScaleY;
    }
    double devicePixelRatio() const
    {
        return _devicePixelRatio;
    }

private:
    OverlayDrawList &_commands;
    OverlayDrawScope _scope;
    Colors _colors;
    double _pixelScaleX, _pixelScaleY, _devicePixelRatio;
    OfxDrawContextHandle _handle;
};

NATRON_NAMESPACE_EXIT
#endif
