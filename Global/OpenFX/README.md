`ofxDrawSuite.h` is the unmodified OpenFX DrawSuite V1 header from
[AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx/blob/adb790bf0043714cae6091d58791e8cdabdd9e12/include/ofxDrawSuite.h),
commit `adb790bf0043714cae6091d58791e8cdabdd9e12` (BSD-3-Clause).
`OverlayInteractV2.h` supplies the matching entry-point property name.

These additions let Natron's HostSupport fork expose OpenFX 1.5 overlays while
its Natron/Nuke parameter extensions remain available. Runtime drawing is
implemented in `Engine/OfxDrawSuite.cpp`; the SDK header has no rendering code.
