// SPDX-License-Identifier: BSD-3-Clause
#ifndef NATRON_GLOBAL_OPENFX_NATRONDRAWSUITE_H
#define NATRON_GLOBAL_OPENFX_NATRONDRAWSUITE_H
#include "Global/OpenFX/ofxDrawSuite.h"

// Optional Natron extension to DrawSuite V1. The same Draw context and lifetime
// apply. This suite provides gradient backgrounds and additive histogram bars.
#define kNatronOfxDrawSuite "NatronOfxDrawSuite"
typedef enum NatronOfxDrawBlendMode {
    kNatronOfxDrawBlendSourceOver,
    kNatronOfxDrawBlendAdd
} NatronOfxDrawBlendMode;
typedef struct NatronOfxDrawSuiteV1 {
    // Four canonical vertices in cyclic order, forming a nondegenerate
    // parallelogram. Vertices 0/1 use colours[0], vertices 2/3 use colours[1].
    // Colours interpolate in premultiplied RGBA. Current colour is unchanged.
    OfxStatus (*drawGradientQuad)(OfxDrawContextHandle context,
                                 const OfxPointD *vertices, const OfxRGBAColourF *colours);
    // State applies to subsequent drawing in this Draw action only.
    OfxStatus (*setBlendMode)(OfxDrawContextHandle context, NatronOfxDrawBlendMode mode);
    OfxStatus (*setAntialiasing)(OfxDrawContextHandle context, int enabled);
} NatronOfxDrawSuiteV1;
#endif
