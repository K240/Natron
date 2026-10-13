`colored-cube.fbx` is a Natron-authored ASCII FBX 7.4 fixture. It has six
quads, two material colors and face-corner UVs; FBX centimeter units become
a two-meter cube in ReadGeo. It is licensed GPL-2.0-or-later.

`blender-cube-binary.fbx` is ufbx's `blender_279_default_7400_binary.fbx`,
copied without changes from commit 26a482ae66871d7de36eb722aa060bce95bce274.
Source: https://github.com/ufbx/ufbx/blob/26a482ae66871d7de36eb722aa060bce95bce274/data/blender_279_default_7400_binary.fbx
It uses upstream's MIT license; see ../../../libs/ufbx/LICENSE (repository root).

`animated-cube.fbx` is a Natron-authored ASCII FBX based on `colored-cube.fbx`,
licensed GPL-2.0-or-later. Its 24 FPS takes are **Move** (1..2 seconds, animated
parent X and child Z), **Scale** (0..1 seconds, child Y scale), and **Hide**
(0..1 seconds, inherited parent visibility). It checks nonzero take beginnings,
hierarchy, retiming and empty geometry frames.

`animated-camera.fbx` is a Natron-authored camera-only ASCII FBX, licensed
GPL-2.0-or-later. Its 24 FPS **CameraMove** take (1..2 seconds) animates a parent
from X=0 to 60 centimeters, the MainCamera Y rotation from 90 to 102 degrees,
and its focal length from 35 to 70 mm. **Hold** preserves the static camera.
The three camera paths are `Rig/MainCamera`, `Rig/OrthoCamera` (4-meter width)
and `Rig/FovCamera` (60-degree horizontal FOV). All cameras are 6 meters from
the origin after conversion. The generic FBX OrthoZoom convention uses a
30-unit multiplier; this fixture uses 13 1/3 in centimeter units.

The following files are copied without changes from the same ufbx commit
`26a482ae66871d7de36eb722aa060bce95bce274` under its MIT license. Their source
directory is [ufbx/data](https://github.com/ufbx/ufbx/tree/26a482ae66871d7de36eb722aa060bce95bce274/data).
See [ufbx LICENSE](../../../libs/ufbx/LICENSE).

| Local file | Upstream file | SHA256 |
| --- | --- | --- |
| animated-skin.fbx | maya_transformed_skin_7700_ascii.fbx | 225589db02340da56c5b36d5c99492525acdb56fc518b7d6fb0743214c8b8759 |
| animated-blend-shape.fbx | maya_blend_shape_cube_7700_ascii.fbx | 2ae340094aa37a9cff77bcdf1b616b065bc7f4c662124ac81bcffec278b51bb9 |
| animated-skin-frame0.obj | maya_transformed_skin_0.obj | 5b45f6f91ab44a49dca9451a6f064bdb98ac1e9646ea09d3345991fe68a18c24 |
| animated-skin-frame8.obj | maya_transformed_skin_8.obj | f0d543a4cc7219ca3ad9ad0af4bea09746b68ece37fc5868ae89b5f92ac6e2a0 |

The OBJ references use centimeters. Their `# ufbx:transform_skin` flag indicates
Maya's mesh-transform-after-skinning convention. Tests compare evaluated
vertices against those references after conversion to meters. A second test
retargets the mesh translation track to joint1 for analytic weighted bone
animation without that convention; the animated demo uses the same retargeting.
