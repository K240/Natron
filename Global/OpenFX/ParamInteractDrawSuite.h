// SPDX-License-Identifier: BSD-3-Clause
#ifndef NATRON_GLOBAL_OPENFX_PARAMINTERACTDRAWSUITE_H
#define NATRON_GLOBAL_OPENFX_PARAMINTERACTDRAWSUITE_H
#include "Global/OpenFX/ofxDrawSuite.h"

// Natron extension, not an OpenFX 1.5 standard property. OpenFX's standard
// kOfxParamPropInteractV1 still requires an OpenGL context.
// Host int[1]: 1 means this version of the native parameter interact is supported.
#define kNatronOfxParamHostPropSupportsDrawSuiteInteract "NatronOfxParamHostPropSupportsDrawSuiteInteract"
// Parameter descriptor pointer[1]: OfxPluginEntryPoint for a complete custom
// control (excluding parametric backgrounds). It replaces the ordinary control.
// Actions use the usual OFX parameter interact ABI and logical pixel coordinates,
// with origin at the bottom left. Only Draw receives kOfxInteractPropDrawContext.
// DrawSuite V1 owns all drawing; no OpenGL context or buffer swap is provided.
// The host clears to the background colour before each Draw.
#define kNatronOfxParamPropInteractDrawSuiteV1 "NatronOfxParamPropInteractDrawSuiteV1"
// Separate host capability and parametric descriptor pointer for backgrounds.
// Only Describe/Create/Destroy/Draw actions are used, with parametric canonical
// coordinates. The host draws its curves and handles after this background.
#define kNatronOfxParamHostPropSupportsDrawSuiteParametricBackground "NatronOfxParamHostPropSupportsDrawSuiteParametricBackground"
#define kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1 "NatronOfxParamPropParametricInteractBackgroundDrawSuiteV1"
#endif
