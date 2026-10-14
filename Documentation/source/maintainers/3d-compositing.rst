.. _maint-3d-compositing:

Native 3D compositing
=====================

Natron's native 3D nodes support image planes, animated cameras, camera
projection, FBX meshes, composed USD stages, parallax and direct-light PBR materials. They are
registered under **3D**. The existing OFX
Card3D remains a separate image transform. The new native Card outputs geometry.

Creating a scene
----------------

Connect images to Card nodes, connect the cards to a Scene, then connect Scene
and Camera to Render3D. Render3D produces ordinary linear, premultiplied RGBA
at the project format and can feed Merge, color correction, Viewer and Write.
Scene accepts sixteen inputs, including nested Scenes; it does not limit the
total number of cards in the CPU renderer.

The node IDs are ``fr.inria.built-in.Card``, ``Camera``, ``Axis``,
``ReadGeo``, ``ReadCamera``, ``TransformGeo``, ``Scene``, ``Project3D``, ``Light``,
``PBRMaterial``, ``ReadUSD``, ``ReadUSDCamera`` and ``Render3D``, each with the same
``fr.inria.built-in.`` prefix. Image, geometry, camera, material, transform and
scene and light connections are checked separately. Dot and Group outputs retain the
upstream data type. Empty Dot and Group inputs can be wired before their source;
choosing that source checks the downstream sockets as well. Project loading
restores connections without checking their intermediate types, so restoration
order does not drop camera connections through Dot.

Coordinates are right-handed: X right, Y up, and a camera looks down its local
-Z axis. A default Camera is at (0, 0, 5). Card sizes are in world units.
Transforms apply scale, then X/Y/Z Euler rotations in degrees, then translation;
the connected Axis is applied outside the local transform. TransformGeo applies
its transform to every Card, imported mesh and light in the incoming scene. Zero scale is invalid.
The focal length and horizontal aperture use the same units (normally mm).
Vertical aperture follows the output aspect ratio, including pixel aspect.
Perspective and orthographic cameras are supported.

FBX mesh import
---------------

Create **3D > ReadGeo** and choose an ASCII or binary FBX in **FBX file**.
Connect ReadGeo to Scene (or directly to Render3D), and Camera to Render3D.
The loader uses vendored **ufbx v0.23.1**, under the MIT license, without the
Autodesk FBX SDK. The version and upstream commit are recorded in
``libs/ufbx/README.natron.md``.

The importer triangulates polygon faces, preserves FBX node hierarchy,
geometric transforms and mesh instances, and converts FBX axis/unit metadata
to right-handed Y-up meters. Per-corner UVs, normals and vertex colors are
retained. Multiple FBX meshes form one editable ReadGeo object; its local
Translate/Rotate/Scale and optional Axis parent apply to the imported model.
Hidden mesh nodes and hidden parents are excluded.

Without an image input, meshes render unlit FBX base/diffuse material colors
and opacity, modulated by vertex colors. Connect a Reader/image to the
**image / material** input to override base color using the first UV set
(repeating outside 0..1). Material opacity and vertex colors still apply.
Connect Project3D instead to project an image onto the triangles using the
same fixed/live camera behavior as Card. Imported texture files are not
automatically opened; use explicit Readers for Natron color management.

Mesh rendering uses a BVH to find triangle intersections on **Vulkan** and
**CPU**. **Renderer: Auto** uses Vulkan when available and falls back to CPU
when a device limit or allocation fails. Both mesh faces render, transparent
surfaces are composited by per-sample depth, and Depth.Z/antialiasing also
work on mixed Card/mesh scenes. Vulkan supports up to sixteen Card/ReadGeo
objects; multiple meshes inside one FBX count as one ReadGeo object.
The triangle/BVH buffers must fit the device's storage-buffer range. Shared
ReadGeo geometry is packed once per render even when instanced. Pure Card
scenes keep their existing Vulkan shader.

The 3D Viewer supports mesh picking, bounding boxes, framing with F,
Translate/Rotate/Scale, and bulk selection with Cards. Filename changes
reload the mesh. ReadGeo caches parsed geometry/BVH between frames; use
the file knob's **Reload** action after replacing a file in place.
Imported FBX lights, external geometry caches, subdivision and PBR parameter
textures are not evaluated. Native Light and PBRMaterial nodes can light imported
meshes. ReadCamera loads cameras from the same
FBX file separately. Natron cameras, Axis and TransformGeo
remain available for composing the imported geometry.

FBX animation
-------------

Enable ReadGeo **Read animation** to sample FBX parent/child transforms,
weighted skinning, blend shapes, material properties and inherited visibility
at the timeline's time. Color, Depth.Z, projection, picking and framing use
the same evaluated geometry. An entirely hidden pose produces empty geometry,
including when the unanimated file pose is hidden. Read animation defaults
to disabled, preserving the unanimated pose of existing ReadGeo projects.

**FBX takes** displays the source FPS, exact take names and source time ranges.
Set **Animation take** to an exact name, or leave it blank for the file's
default take. An unknown name reports an error while Read animation is enabled.
The take's beginning is aligned to **Start frame** (default: 1). Playback uses
the project's FPS, so source FPS is informational; a 24 FPS file plays in
real time in a 48 FPS project. The source time in seconds is::

    take_begin + (timeline_frame - start_frame) / project_fps * playback_speed + time_offset

**Playback speed** defaults to 1; 0 holds a pose and negative values reverse
playback. **Time offset (seconds)** defaults to 0. Both parameters can be
animated and are evaluated at the current timeline frame. A varying speed
scales the current frame's time by its value; it is not accumulated over earlier
frames. Times outside the take range follow the FBX curves' extrapolation.
Changing the project FPS invalidates rendered images as well as metadata.

**Apply mesh transform to skin** supports exports that animate the skinned
mesh node independently of its bones, including Maya mesh transform animation.
It applies that node's transform relative to its mesh bind pose after skinning.
Enable it only for that convention; default skinning already places weighted
vertices through the bones in world space. The option requires a mesh bind pose
and reports a missing pose as an error. Ordinary bone animation leaves it off.

ReadGeo retains the parsed ufbx scene and caches its latest evaluated pose by
source time, take and skin transform option. Deformation and BVH construction
run on the CPU when the pose changes; Vulkan then renders the resulting mesh.
Repeated color/depth sampling at the same pose reuses its geometry. Filename,
file size and modification time changes reload the asset; **Reload** also
handles replacements that retain size and timestamp. ReadGeo's own transform
and its Axis/TransformGeo parents apply outside the imported animation.

FBX cameras
-----------

Create **3D > ReadCamera**, choose an ASCII/binary **FBX file**, and connect its
camera output to Render3D or Project3D. Camera-only FBX files are supported;
this path skips loading mesh buffers. **FBX cameras** lists camera node hierarchy
paths, including instances. Set **FBX camera** to a listed path or a unique
node name; a blank value chooses the first camera. Missing cameras, ambiguous
names and unknown animation takes report errors.

ReadCamera preserves FBX camera/parent transforms, lens animation, perspective
or orthographic projection, and clip distances. The imported coordinates use
right-handed Y-up meters and local -Z forward. Horizontal framing follows the
FBX field of view and film gate; vertical framing follows the project's output
aspect in Render3D or the image format in Project3D. Set that format to the
source camera's aspect when matching a plate. Look-at constraints, camera
switchers, film offsets and lens distortion are not evaluated.

**Read animation** defaults to enabled on ReadCamera. **Animation take**, **Start
frame**, **Playback speed** and **Time offset (seconds)** use the same project-FPS
mapping as ReadGeo; disabling animation uses the unanimated camera pose. The
parsed FBX and latest sampled camera are cached and **Reload** replaces them.

**Translate offset** adds to the imported position in the optional parent Axis
space. **Rotate offset** rotates the camera basis without orbiting its imported
position; **Scale multiplier** scales that basis. The Axis transform then applies
outside these adjustments. These offset knobs are animatable and support
Gizmo editing and Undo/Redo in the 3D Viewer. **Focal multiplier** multiplies the
imported perspective focal length, or divides the orthographic width; 1 preserves
the imported horizontal framing. Project3D's fixed/live projection also samples
ReadCamera at the requested reference/output frame.

USD scene import
----------------

Create **3D > ReadUSD**, choose a ``.usd``, ``.usda``, ``.usdc`` or ``.usdz``
file, and connect its scene output to Scene or directly to Render3D. Create
**ReadUSDCamera**, select the same file and a camera path from **Source info**,
and connect it to Render3D or Project3D. The built-in **OpenUSD 26.5** stage
runtime handles references, sublayers, payloads and ordinary scene instances.
It feeds the existing native Scene, CPU/Vulkan renderer and 3D Viewer.

The Windows build script enables ``NATRON_ENABLE_USD`` and installs OpenUSD via
vcpkg without its Python or imaging dependencies. Other builds can enable the
same CMake option and provide the ``pxr`` package. With the option disabled,
these nodes report that OpenUSD support is unavailable. OpenUSD needs its schema
and file-format plugin resources at runtime: the Windows runner configures
``PXR_PLUGINPATH_NAME`` for ``usd/*/resources/`` beside each executable. These resources are copied next to
the same DLLs used by that executable to avoid loading duplicate USD libraries.
A custom installation must supply the corresponding resources or plugin path.

ReadUSD preserves mesh hierarchy transforms, ``resetXformStack``, inherited
visibility, vertex/varying/uniform/face-varying primvars and indexed ``st`` UVs.
Polygon faces, including concave faces, are triangulated; holes are omitted.
Stage ``upAxis`` (Y or Z) and ``metersPerUnit`` are converted to right-handed
Y-up meters. Render/default purpose meshes are included; guide/proxy purpose
meshes are omitted. Ordinary instance proxies are traversed. Instances currently
receive their own evaluated mesh/BVH rather than sharing a persistent GPU asset.

**Prim subtree** limits geometry/lights to an absolute prim path; blank or ``/``
reads the stage. **Variant selections** accepts one ``/Prim set=selection`` per
line. Selections are authored only in a private session layer, and source files
are unchanged. Copy the same overrides to ReadUSDCamera when variants change
camera contents. **Load payloads** chooses OpenUSD LoadAll/LoadNone at opening.
The node's Translate/Rotate/Scale and optional Axis move the entire imported
subtree, including its lights. Individual USD prim editing is not exposed yet.
The Viewer selects/frames the stage as one ReadUSD object, with a shared Gizmo
for all its meshes; ReadUSDCamera has the usual editable camera offset controls.

**Read animation** defaults to enabled and evaluates USD time samples. Source
seconds are ``(frame - Start frame) / project FPS * Playback speed + Time offset``;
the source time code is ``startTimeCode + seconds * timeCodesPerSecond``.
Disabled animation holds ``startTimeCode``. Camera parent transforms, focal
length, aperture, clipping and perspective/orthographic settings use the same
time mapping. USD lens/aperture values in tenths of a scene unit are converted
to millimeters; clipping and orthographic width use meters. Horizontal framing
is retained; the output format defines vertical aspect. Film offsets and
lens effects are not evaluated.

UsdShade inherited/collection material bindings and per-face material subsets
map **UsdPreviewSurface** authored diffuseColor, opacity, emissiveColor,
metallic, roughness and ior values to the shared native Material. Imported
PreviewSurface materials are lit. Without a supported binding, displayColor and
displayOpacity render unlit. The optional image/PBRMaterial/Project3D input
uses the existing UV or camera-projection pipeline on every imported mesh.
**diffuseColor** can read the ``rgb`` output of **UsdUVTexture**. PNG/JPEG and
formats supplied by installed Qt image plugins are decoded through OpenUSD's
asset resolver, including images embedded in USDZ and paths authored on material
interfaces or referenced layers. **opacity** can read ``a/r/g/b`` from the same
texture reader; without this connection, file alpha does not affect opacity.
An explicit native image/material assignment replaces the imported texture
bindings, including normal/roughness/metallic maps; disconnecting that input restores the USD material.
``sourceColorSpace`` supports ``sRGB``, ``raw`` and ``auto`` following the
PreviewSurface transfer-curve rule. RGB is filtered in linear straight form and
premultiplied with opacity for compositing. Scale/bias and texture fallback are
applied; unreadable images report a Source info warning. EXR/HDR and UDIM images
are not supported by this initial decoder.

**normal** reads ``UsdUVTexture.outputs:rgb`` as a signed tangent-space vector.
For an 8-bit normal map, author ``sourceColorSpace = "raw"``,
``scale = (2,2,2,1)`` and ``bias = (-1,-1,-1,0)`` on the reader, as required by
`the PreviewSurface specification <https://openusd.org/release/spec_usdpreviewsurface.html>`_.
The tangent basis comes from the normal map's UV set before its sampler transform;
positive Y follows increasing V. Mirrored UVs and object transforms retain basis
handedness, and nonuniform scales use world-space tangents with inverse-transpose
normals. Degenerate UVs or zero sampled vectors retain the geometric shading
normal. Normal maps affect lighting only; they do not displace geometry or change
Depth.Z, opacity or shadow-ray bias.

**roughness** and **metallic** read individual ``r/g/b/a`` outputs, then clamp to
``[0,1]`` for the shared GGX shader. Data maps retain independent alpha and signed
scale/bias values instead of premultiplying with file alpha. Set their color space
to ``raw`` for linear data. Packed ORM images can share a reader (G roughness,
B metallic); occlusion is not evaluated. Every map can use its own UV primvar,
transform, wrapping and animated sampler inputs. The same renderer samplers are used by imported USD bindings and native
PBRMaterial map inputs.

Texture coordinates support **UsdPrimvarReader_float2**, named/indexed primvars,
material interface values, and chained **UsdTransform2d** scale/rotation/translation.
The reader fallback is used when a named UV primvar is absent. Per-axis black,
clamp, repeat and mirror addressing use bilinear filtering; ``useMetadata`` reads
Qt image text fields ``wrapS``/``wrapT`` and otherwise uses black. Animated file,
UV and sampler inputs are evaluated at source time. Image groups from per-face
materials share the stage Gizmo, but count separately against Vulkan's sixteen
geometry-object limit. Texture samplers are shared across objects and map
channels, with a separate limit of sixteen distinct samplers. Auto selects CPU
above either limit. Immutable images are
cached until **Reload file**; reload also picks up external image edits.

Other connected shader inputs use authored fallback values and diagnostics.
MaterialX, full shader graphs, separate opacity maps, displacement, clearcoat and specular workflow are not implemented. Values
are interpreted as linear RGB in the project working space; USD gamut metadata
and arbitrary texture ICC profiles are not converted to an OCIO working gamut.
Subdivision surfaces render their polygon control mesh. USD skeletons,
blend shapes, PointInstancer, curves and implicit primitives are not evaluated;
convert geometry to baked polygon Mesh where needed.

DistantLight uses its angular integrated intensity (including normalize and
exposure), direction and color. SphereLight is approximated as a point source,
or a spot with ShapingAPI coneAngle/coneSoftness. Normalized sphere intensity
maps to one quarter of native point strength; unnormalized intensity maps to
``pi * radiusInMeters^2 * uniformScale^2`` times native strength. Nonuniform
sphere transforms use the X-axis scale approximation. Color temperature and
shadowEnable are supported. Direct-light shadows are hard; area shape,
IES/focus, per-lobe multipliers and linking are not evaluated. DomeLight uses
the shared environment-light integration with color, intensity/exposure,
color temperature, shadowEnable, transform and animated texture assets.
``automatic`` and ``latlong`` texture formats use latitude-longitude mapping;
other formats disable the dome with a diagnostic. Its local +Y pole and +Z
image center follow the OpenUSD/OpenEXR convention. Translation and scale are
ignored, including the stage's meter conversion; rotation and mirrored
orientation remain effective. Dome ``normalize`` is ignored. Images use the
same Qt/Ar decoder as surface maps, including USDZ-embedded PNG/JPEG images.
The texture asset's ``colorSpace`` metadata supports raw/linear/sRGB/auto;
auto follows the surface texture rules. Alpha is ignored. A missing texture
asset emits uniform color; an unreadable authored file emits black and reports
a warning. USD-authored EXR/HDR files remain unsupported by this decoder;
native Environment lights can use an EXR/HDR Reader instead. Portals do not
guide sampling. Other
USD light types are omitted with a diagnostic. Imported lights currently have
no separate editor icons; the ReadUSD transform controls them together.

**Source info** lists source timing, cameras, initial meshes/lights and
unsupported elements/composition errors. Diagnostics are sampled at the source
start time. Root filename/size/mtime changes invalidate the node cache. Use the
file knob's **Reload** after updating referenced layers or other dependencies;
it forces disk reload even when a filesystem timestamp did not advance.
Parsed stages and the most recent mesh pose are cached. There is no automatic
watcher for every USD dependency yet.

Lighting and PBR materials
--------------------------

Connect **3D > Light** to a Scene input alongside Card/ReadGeo geometry.
Connect **3D > PBRMaterial** to a Card/ReadGeo **image / material** input to
enable lighting. Image or Project3D inputs connected directly to geometry remain
unlit, preserving existing compositing projects. A PBR Card can render without
an image. Geometry, cameras, materials and lights share the same Scene, 3D
Viewer and Render3D; ReadUSD also fills these structures using composed OpenUSD data.

PBRMaterial uses GGX/Trowbridge-Reitz, height-correlated Smith visibility and
Schlick Fresnel with an energy-conserving diffuse component. **Metallic** and
**Roughness** are in 0..1; IOR >= 1 determines dielectric reflectance. Roughness
zero uses a numerical floor of 0.04. **Base color multiplier** is straight linear
RGB, multiplying the optional image/projection or imported FBX surface/vertex
colors. Connect a white Constant through PBRMaterial to replace imported RGB.
**Opacity** multiplies texture, vertex, imported surface and geometry opacity.
**Emission** adds linear RGB independent of lights. Lighting is evaluated in
straight RGB then premultiplied for Natron compositing. With no lights only
emission is visible; there is no implicit headlight or ambient illumination.
PBRMaterial retains its existing input 0 (**image / material**) and adds three
optional image inputs: 1 **normal**, 2 **roughness**, 3 **metallic**. Connected
roughness/metallic maps replace their numeric controls, with independent
**Roughness channel** and **Metallic channel** selectors (R by default). A packed
ORM image can feed both sockets with G and B selected. Disconnect a map to
restore its numeric control. Data-map alpha does not change surface opacity.

**Normal encoding** defaults to **0..1 RGB**, decoded as ``2 * RGB - 1``.
Select **-1..1 RGB** for an already decoded signed vector. **Invert normal Y**
negates its tangent Y component; positive Y follows increasing V. Normal maps
use Card UVs or the ordinary UVs of an imported mesh, independent of the
Project3D camera and of the separate UV sets on imported USD material maps.
**Map wrapping** selects black/clamp/repeat/mirror for all three data maps
(repeat by default). Image format dimensions define the UV domain; a cropped
image window stays in its original position and the missing data is zero.
Zero normals or degenerate UV bases retain the geometric shading normal.

Data channels use the values produced by the image graph without an additional
color conversion, premultiplication or unpremultiplication. Configure map
Readers as raw/linear data with unpremultiplied output; a premultiplication or
color transform earlier in the graph changes those values. RGB inputs provide
alpha 1, and a single-channel input supplies the same scalar to each channel.
Map inputs and all map controls follow the output time, even when the base image
and projection are frozen at a Project3D reference frame. Normal maps affect
lighting only; alpha, Depth.Z and shadow geometry remain unchanged.

The nearest PBRMaterial in a material chain supplies all surface parameters
and map bindings. Upstream PBRMaterial nodes are unwrapped only to find the base
image or projection. An explicit PBRMaterial assignment on ReadUSD replaces
its imported surface maps; disconnecting the material restores the USD bindings.
Existing projects keep their input-0 connections and the new maps are optional.

Light supports **Distant**, **Point**, **Spot** and **Environment**. Distant and Spot point down
local -Z; Translate/Rotate/Scale, optional Axis parents and scene transforms
are evaluated at the timeline frame. Intensity is multiplied by
``2 ** Exposure``. Point/Spot use inverse-square world-distance attenuation;
these initial native strength values are renderer units, not calibrated USD
photometric units. Spot **Cone half angle** is in degrees; **Cone softness**
controls the smooth transition between its inner and outer cone.

**Environment** integrates diffuse lighting and GGX reflection over an optional
latitude-longitude image. Connect a Reader/image graph to Light input 1
(**environment image**); input 0 retains its existing optional Axis connection.
Without an image it emits uniform **Color**. Color, intensity and exposure
multiply the image radiance. Use straight linear RGB; alpha does not mask the
light. Configure a Reader's input/output spaces as linear for a linear EXR/HDR,
and its file/output premultiplication as unpremultiplied. Natron Reader formats,
including float EXR/HDR, are supported and values above 1 are preserved. Image
graphs follow output time. The full source format defines the map; cropped
data windows keep their positions and missing regions emit black.

Rotate the Light or its Axis to orient the map. Translation and scale do not
affect its illumination. The local +Y pole is at the image top; +Z is at its
center and +X at one quarter of its width, following the
`OpenUSD DomeLight convention <https://openusd.org/release/user_guides/schemas/usdLux/DomeLight.html>`_.
Longitude repeats across the seam and latitude clamps at the poles.
**Environment samples per lobe** selects 16/32/64 samples (32 by default),
independently integrating a cosine-weighted diffuse lobe and a GGX half-vector
specular lobe. Samples are deterministic for stable playback. This is finite
quadrature: small bright HDR features and strong occlusion can show integration
artifacts; increase samples or soften the source environment where necessary.
Roughness, metallic and normal maps affect these lobes through the same surface
evaluation as direct lights. **Cast shadows** tests each sampled direction with
the shared alpha-aware geometry routines, allowing environment occlusion and
sampled soft shadows. It increases render cost. Environment images illuminate
surfaces; empty pixels remain transparent and Render3D's background input
continues to provide the compositing background. There are no interreflection
or indirect-bounce rays.

**Cast shadows** enables hard shadows using the existing mesh BVH and Card
intersection routines. Texture/projection alpha and geometry/material opacity
attenuate shadow rays. **Shadow bias** offsets the surface in world units;
adjust it for unusually large or small scenes. Per-corner normals are
interpolated and transformed by inverse transpose; missing normals use the
triangle normal. Both sides shade toward the viewer. Depth.Z and its coverage
are independent of the light strength. Light icons can be picked and moved or
rotated with the existing Gizmo and Undo controls.

CPU and Vulkan support the same direct-lighting model. Vulkan supports sixteen
geometry objects and sixteen lights for lit scenes; Auto falls back to CPU for
larger scenes. Surface images, data maps and environment images share the
sixteen-distinct-sampler limit; Auto also falls back above that limit.
Area-light shape/soft shadows, indirect light, environment importance sampling
and general shader networks are planned extensions. This is direct and
environment PBR lighting, not full
UsdPreviewSurface compatibility.

Fixed camera projection
-----------------------

Connect the source image and a projection Camera to Project3D, then connect its
material output to Card or ReadGeo. Use a second Camera on Render3D to view the result.
**Freeze projection camera** samples the projector at **Reference frame**
(default: 1). **Image timing** independently selects Reference frame or Current
frame; its default Follow camera preserves the original combined Freeze
behavior in existing projects. The render camera and geometry transforms
continue to animate at the output frame.

Pixels outside the projected image or outside the projection camera's near/far
range are transparent. Both faces of the card receive the projection. The
image's output format defines the projection aspect; its finite data window
provides the texture pixels. Insert Crop or Reformat before a texture with an
infinite region of definition.
Cropping the data window preserves its position within the source format,
instead of stretching the cropped pixels across the projection.

Viewer and editing
------------------

Connect Render3D, or a downstream 2D composite, to a Viewer and press **3D**.
The Viewer discovers the first upstream Render3D. Press **Camera**, or H, to
look through the render camera; the perspective inspection camera has its own
transform and lens, independent of the render settings. Switching the inspection
view does not edit the scene.

* Alt + left drag: orbit.
* Middle drag: pan.
* Mouse wheel: dolly.
* Click a Card/mesh or a Camera/Axis/TransformGeo icon: select it in the graph and
  open its parameter panel. Selecting one of these nodes in the graph also
  selects its handles in the 3D Viewer.
  The selected object's name is displayed; hovering over another object's
  icon identifies it without overlapping scene labels.
* Shift + click: add or remove an object from the selection. Drag from empty
  space to select geometry intersecting the rectangle; Shift + drag adds geometry.
  Rectangle selection includes overlapping cards. Esc cancels the rectangle.
  Multiple node selections are synchronized with the graph in both directions.
  Handle endpoints take priority over nearby icons; Shift + click selects an
  object underneath a handle. Clicking empty space clears selection across
  the participating Group graphs.
* W / E / R, or the tool menu: select Move / Rotate / Scale.
* Move: drag an X/Y/Z handle to edit translation along that parent-space axis.
* Rotate: drag an X/Y/Z ring to edit that Euler rotation in degrees.
* Scale: drag an axis handle to scale that component; drag the center square
  to scale all three components together.
* Esc during a handle drag: cancel that gesture.
* F: enter the inspection camera and frame the selected geometry, including
  all placements of a reused node. With no selection, frame the entire scene.
  Selecting a camera or parent transform also includes the scene and the
  selected editor origins. The orbit pivot is the world-space bounds center;
  FBX meshes use their current evaluated pose rather than their node origins.
  The fit preserves the inspection orientation, accounts for viewport aspect
  and corner depth, and leaves a margin around the geometry. Hidden/empty meshes
  do not enlarge the bounds. Point-only selections use their editor origins;
  a scene with no geometry or selection resets the inspection pivot.

Gizmos are anchored at the projected object origin and drawn as a fixed-size
overlay. Panning, dollying and lens changes move the anchor without warping
the handles or rotation rings. Camera rotation foreshortens the axes/rings;
parent scale and shear do not stretch the rotation rings. Rotation dragging
uses the displayed ring's projection, including the edge-on fallback, so
the visible arc and its picking/angle calculation stay aligned.

The handles work on Card, ReadGeo, ReadUSD, Camera, ReadCamera, ReadUSDCamera,
Light, Axis and TransformGeo. They account for the
parent Axis and enclosing TransformGeo nodes. Editing an Axis or TransformGeo
changes its connected scene while leaving each child's local knobs intact.
Each drag is one Undo/Redo action and uses the existing auto-keying behavior.
Undo restores an existing key's interpolation and derivatives, or removes a
newly added key. Redo edits the original frame even after seeking elsewhere.
Selecting several Cards provides bulk Move / Rotate / Scale controls. Move uses
world axes at the selection center and converts the same world displacement
into each card's parent coordinates. Rotate adds the same local Euler angle to
each card, and Scale applies the same local factor, both at each card's own
center. The last selected card provides their rotation/scale handles. All cards
in a gesture share one Undo action on that card's parameter-panel stack.
Only changed translation/rotation/scale components receive automatic keys.
Reusing a Card in multiple scene branches selects and edits that node once;
every placement inherits its knob changes through its respective parent.
Mixed selections of Cards, cameras and parent transforms show selection guides
and support framing; they do not expose bulk handles. This avoids applying a
parent transform and its child's local transform in the same gesture.
Camera lens and projection settings are animated through their normal parameter
panels and Curve Editor. Viewer display uses the existing Linear,
sRGB, Rec.709 or BT1886 LUT selection, gain and gamma.

Render camera frusta are blue; projection camera frusta are purple. Their
width and height follow focal length, aperture, orthographic width and the
output or source aspect ratio. The displayed depth reaches the scene rather
than the camera's potentially very distant far clip plane. Frusta are guides
and do not become geometry in Render3D.

Frozen projection cameras and their parent axes are displayed at the reference
frame in world space, even inside TransformGeo. Dashed guides and a frame label
identify samples from another frame. Their handles are available when the
playhead is at that reference frame; at other frames, selecting them opens the
parameter panel and shows a reference-frame message. Live projection cameras
can be manipulated at the current frame. This prevents accidentally editing
the wrong frame while inspecting a fixed projection.

Depth output
------------

Render3D produces a one-channel **Depth.Z** layer alongside its color image.
It stores positive camera-local -Z of the closest surface whose sampled texture
alpha and object opacity are nonzero. Completely transparent texels, zero-opacity
geometry, pixels outside the projected image, and camera clipping do not contribute.
Empty pixels are zero. Semi-transparent foreground cards supply their own
depth, rather than a blend with the surface behind them.

Z is axial depth, not Euclidean distance along the viewing ray. It follows the
current render camera, including perspective/orthographic projection and parent
transforms. Scaling a camera or its parent changes the camera-local depth units.
Antialiasing stores the nearest contributing sample depth without averaging it
with zero at silhouettes. Background images do not receive a geometry depth.
This is a single nearest-surface layer, not a deep image with multiple samples.

Choose **Output: Depth preview** to visualize the depth as near-white / far-black
RGBA in both the 2D and 3D Viewers. **Depth preview range** supplies the near and
far values and can be animated; it requires ``0 <= near < far``. The preview
uses sample coverage for its alpha and premultiplication. Display LUT, gain and
gamma still apply. Color output remains the default. The preview and its range
do not change clipping or the raw Depth.Z layer. In the perspective inspection
view, depth is measured from that inspection camera.

Select Depth.Z in a Viewer layer menu or route its Z channel through Shuffle
for 2D depth processing. Use a float EXR to preserve its values. In the OIIO
Writer, enable **All Planes** and select **32f** to save color and Depth.Z
together; PNG and a normalized depth preview do not preserve raw Z values.

Rendering implementation and limits
-----------------------------------

The CPU and Vulkan implementations find ray/plane intersections per pixel,
sort those intersections by depth and composite front-to-back. This handles
intersecting transparent cards without relying on a whole-object depth sort.
Textures use bilinear interpolation; antialiasing is selectable as 1, 2x2 or
4x4 samples per pixel. HDR values are retained in the render output.
CPU rendering prepares inverse card/projector transforms once per frame and
shares each sample's camera ray across the cards.

Auto uses Vulkan when available, and falls back to CPU on unavailable hardware,
allocation/driver errors, device mesh-buffer limits, or scenes containing
more than sixteen geometry objects or lights in lit scenes.
CPU supports larger scenes. Explicit Vulkan selection reports failures instead
of switching renderers. The inspection Viewer also falls back to CPU.

This first implementation renders a complete output frame. It snapshots image
inputs into float RGBA, uploads those textures for Vulkan rendering, then reads
the result back to the Engine's image cache. It does not yet keep the entire 3D
pipeline in GPU memory. A background input is fitted to the project output.
Mesh buffers are uploaded for each render; parsed FBX assets, static BVHs,
the latest animated pose and Vulkan programs are reused. GPU intersections use float precision;
CPU intersections use double precision. Vulkan mesh textures use explicit
float bilinear interpolation to avoid hardware interpolation quantization.
Environment/area lighting, motion blur, additional geometry passes and richer
USD shader/deformation support are future extensions.

Validation
----------

``NatronCardScene`` runs analytic CPU checks for alpha, crossing cards, fixed
projection, HDR, transforms, empty scenes and cancellation. Adding ``--gpu N``
compares 552,960 pixels with the selected Vulkan adapter, including color and
depth passes, sixteen textures, three antialiasing settings and
perspective/orthographic cameras. Analytic depth checks cover transparency,
clipping, transformed cameras and undiluted silhouette depth.

On Windows, use::

    .\tools\run-windows-qt6.ps1 -Program Scene3DSmoke
    .\tools\run-windows-qt6.ps1 -Program Scene3DGuiSmoke -GuiTimeoutSeconds 120

The first test exercises real image inputs, typed connections, Axis,
TransformGeo, camera animation, fixed/live projection, backgrounds and saved
project reload with pixel comparisons. The second exercises real Viewer
events, picking, handles, Undo/Redo and animated edits. Both use an isolated
test profile. Set ``NATRON_VULKAN_DEVICE`` to select another adapter.
The Viewer test includes separate gestures, uniform scaling, cancellation,
parent transforms, Camera/Axis/TransformGeo manipulation, graph selection,
perspective/orthographic frusta, fixed/live projector sampling and Undo/Redo
after moving the playhead. It also covers Shift/rectangle selection, bulk
transforms under different rotated and nonuniformly scaled parents, automatic
keys on changed axes, whole-gesture cancellation, individual rotation/scale
pivots, reused Card nodes and mixed parent/child selection safety.
Resized viewports and simultaneous edits inside and outside a Group are
included, along with Card/Axis/icon picking near overlapping handles.
After the Viewer test, set ``NATRON_SCENE3D_RELOAD_GUI=1`` and run
``Scene3DSmoke`` to reload its saved bulk animation in NatronRenderer, check the
original key values and compare CPU/Vulkan output without a GUI.
``NatronFbxMesh`` / CTest ``FbxMeshCpu`` validates ASCII/binary loading,
triangulation, meter conversion, geometric/node transforms, Unicode paths,
materials, barycentric UVs, projection, mixed transparent geometry, clipping,
Depth.Z and error handling. Animation checks include nonzero take beginnings,
parent/child motion, named takes, visibility, bone animation, Maya skin
transform convention, exported OBJ references, normals and blend-shape offsets.
Opaque Maya materials with a black transparency color are also covered.
Adding ``--gpu N`` compares CPU and Vulkan color/depth output for imported
meshes, UVs, cropped projection, vertex/material alpha, HDR, sixteen shared
instances, three antialiasing settings and perspective/orthographic cameras.
It also checks clipped back faces, shared edges, more than sixteen transparent
surfaces inside a mesh, buffer reuse and Vulkan validation errors.
Animated, skinned, blended and entirely hidden poses are included in those
comparisons on both NVIDIA and AMD adapters.
The engine test also checks native ReadGeo import, UV/image overrides,
Axis/TransformGeo, fixed/live projection, node animation and save/reload.
FBX animation checks exercise the real image cache, FPS changes, start frame,
speed/hold/reverse, offset, named takes, empty visibility frames, imported
deformation and saved playback settings. Static pose comparisons use float
EXR because the PNG writer's dithering varies with the requested frame.
It exports color and Depth.Z to float EXR, checks exact
foreground/background/empty Z values, exercises depth previews, and reloads an
animated camera and saved preview settings. It uses ``oiiotool`` from PATH or
the bundled Windows build runtime; set ``OIIO_TOOL`` for another location.
The Viewer test checks depth preview range changes, editing/Undo in depth mode,
invalid-range recovery and returning to the original color view.
It also checks imported FBX animation after timeline seeks, edits/Undo at an
animated pose, framing, hold/take controls and invalid-take recovery. Framing
checks cover offset animated bounds, the orbit pivot, selected versus whole
scene fits, wide/tall viewports, millimeter meshes and entirely hidden poses.
Camera import checks include camera-only ASCII and Blender binary FBX, unit/axis
conversion, parent/rotation/lens animation, clip distances, orthographic width
and horizontal FOV mode. Render3D color/Depth.Z is compared to equivalent native
cameras; FPS invalidation, retiming and project round trips are checked. Viewer
tests cover imported frusta, offset Gizmos/Undo, error recovery and fixed/live
ReadCamera projection at reference frames.
``NatronCardScene --benchmark`` measures a sixteen-card 640x360 CPU scene with
2x2 samples, reporting the median of five renders after a warm-up.

To create an editable three-layer sample project, run::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings', '--interpreter', 'tools/create-scene3d-demo.py')

The project and its image assets are saved to ``build/scene3d-demo``.

FBX test scene and execution
----------------------------

From the repository root, generate the editable FBX example and add its
Viewer/graph layout::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings', '--interpreter', 'tools/create-scene3d-fbx-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DFbxDemoGuiSmoke -GuiTimeoutSeconds 120

Open the prepared project::

    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-fbx-demo/fbx-meshes.ntp')

``FbxViewer`` is connected to ``FbxRender3D``. Press **3D**, then **Camera**
to inspect the render-camera view. Select either ReadGeo and use W/E/R to
move/rotate/scale; F frames the selected geometry in the inspection view.
Frames 1..48 animate the render camera and one mesh rotation. Set Render3D
**Output: Depth preview** to inspect depth with the preconfigured 5..10 range.

The scene contains an ASCII FBX cube with two material colors and an Axis
parent, a binary FBX cube with frozen Project3D checkerboard projection,
and a background Card. Copied FBXs, the projection PNG, ``color.png``,
``depth-preview.png`` and five-channel float ``color-and-depth.exr`` are
saved beside ``fbx-meshes.ntp`` in ``build/scene3d-fbx-demo``.
The GUI preparation test verifies that the saved scene loads, both meshes
are present and its connected Viewer renders through Vulkan (or CPU with
``NATRON_VULKAN_DISABLED=1``). Regenerating
the example replaces its saved scene and generated preview files.

Run regression checks::

    .\tools\run-windows-qt6.ps1 -Program CTest
    .\build\windows-qt6\Tests\NatronFbxMesh.exe --gpu 0
    .\tools\run-windows-qt6.ps1 -Program Scene3DSmoke
    .\tools\run-windows-qt6.ps1 -Program Scene3DGuiSmoke -GuiTimeoutSeconds 120

For the CPU-only integration run, set ``$env:NATRON_VULKAN_DISABLED='1'``
before Scene3DSmoke and remove it afterwards with
``Remove-Item Env:NATRON_VULKAN_DISABLED``.

Animated FBX test scene
-----------------------

Generate and prepare the animation example from the repository root::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings', '--interpreter', 'tools/create-scene3d-fbx-animation-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DFbxAnimationDemoGuiSmoke -GuiTimeoutSeconds 120

Open the saved project::

    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-fbx-animation-demo/fbx-animation.ntp')

In **FbxViewer**, press **3D**, then **Camera**, and scrub frames 1..49 at
24 FPS. The left **HierarchyAnimation** cube moves through its imported
parent/child animation; change its take from Move to Scale or Hide to compare
tracks. The center **BoneAnimation** uses weighted vertices animated through
the root joint. The right **BlendShapeAnimation** deforms under a frozen
checkerboard projection. Read animation is already enabled on all three.
Select a ReadGeo to inspect its take/time controls or use W/E/R to edit its
outer transform. F frames its current animated bounds in the inspection view.
Render3D **Output: Depth preview** uses a preconfigured 5..10 depth range.

The generator copies the FBXs and writes a derived ``animated-bones.fbx``
by retargeting the skin fixture's mesh translation track to its root joint.
It also renders ``color-0001.png``, ``color-0025.png``, ``color-0049.png``,
``depth-preview.png`` and float ``color-and-depth.exr`` alongside the project.
The GUI preparation checks playback at all three frames, saved Read animation
controls and the compact toolbar through window resizes in both 2D and 3D.
Regenerating the example replaces its generated files.

Imported camera example
-----------------------

To generate and prepare an editable ReadCamera test scene, run::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings', '--interpreter', 'tools/create-scene3d-fbx-camera-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DFbxCameraDemoGuiSmoke
    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-fbx-camera-demo/fbx-camera.ntp')

In **FbxViewer**, press **3D**, then **Camera**, and scrub frames **1..25**.
**AnimatedCamera** is a ReadCamera that plays FBX parent translation, camera
rotation and focal length. **ReferenceCamera** is a second ReadCamera using the
unanimated pose for **FrozenProjection**. Both read the copied camera-only
``animated-camera.fbx``. The material-colored and projected cubes show the
resulting parallax and lens changes. Select AnimatedCamera and press F to
inspect its frustum; use W/E/R to adjust its transform offsets.

The generator writes ``color-0001.png``, ``color-0013.png``, ``color-0025.png``,
``depth-preview.png`` and float ``color-and-depth.exr`` beside the project.
GUI preparation adds the Viewer, arranges the graph, verifies playback and
saves the project at frame 13. The main Scene3DGuiSmoke also saves an imported
camera projection test at ``build/scene3d-gui-smoke/imported-camera-scene.ntp``.

Lighting validation and example
-------------------------------

``NatronLighting`` checks Lambert/GGX reference values, metal/dielectric
reflectance, exposure, inverse-square attenuation, spot cones, hard/alpha
shadows, emission, premultiplication, unlit compatibility, Depth.Z and invalid
parameters. ``--gpu N`` compares those models plus projection, smooth/missing
normals, per-face surfaces, HDR, mirrored/nonuniform transforms and sixteen
lights on a Vulkan adapter, including validation-layer checks.

On Windows, run engine and Viewer checks with::

    .\tools\run-windows-qt6.ps1 -Program Scene3DLightingSmoke
    .\tools\run-windows-qt6.ps1 -Program Scene3DGuiSmoke -GuiTimeoutSeconds 120

``Scene3DLightingSmoke`` validates native node sockets, Axis parenting, all
three light types, emission/alpha, animated exposure, a Project3D wrapped by
PBRMaterial, native map sockets, RGBA channels, signed/unsigned normals, Y flip,
raw Reader data, FBX/USD overrides, map timing under projection Freeze, map
serialization and float CPU/Vulkan color/depth comparison, Auto's seventeen-light
CPU fallback and project save/reload. The GUI smoke also picks and edits a
Light with move/rotate Gizmos and Undo.

Create, prepare and open the example with::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings','--interpreter','tools/create-scene3d-lighting-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DLightingDemoGuiSmoke
    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-lighting-demo/lighting.ntp')

The example combines two FBX cubes with plastic/metal materials, a PBR Card
floor, an animated Distant light and a Point fill light. Open LightingViewer,
select **3D** and **Camera**, and scrub frames 1..25. Select KeyLight to move
or rotate it with W/E and Undo. The directory includes PNGs at frames 1/13/25,
a depth preview and float color-and-depth EXR. It requires the existing OFX
Constant and image writer plugins.

The lighting example's BluePlasticMaterial includes **NormalMap** and
**PackedORM** Readers connected to inputs 1/2/3. They use linear, unpremultiplied
data; roughness selects G and metallic selects B. The gold material keeps its
numeric controls. Open ``build/scene3d-lighting-demo/lighting.ntp`` after running
the example generator and Viewer preparation above.

Environment validation and example
----------------------------------

``NatronLighting`` also checks uniform diffuse/mirror environment references,
HDR radiance, image orientation/poles, ignored alpha/translation/scale,
roughness, environment occlusion with translucent blockers, unchanged
unlit/depth output, all three sampling counts and the combined sampler limit.
``NatronUsdScene`` checks DomeLight time/transform/exposure, raw/auto color,
missing/unsupported assets, embedded USDZ textures and external image reload.

Run the node and Viewer checks with::

    .\tools\run-windows-qt6.ps1 -Program Scene3DEnvironmentSmoke
    .\tools\run-windows-qt6.ps1 -Program Scene3DEnvironmentDemoGuiSmoke

The engine check compares an HDR EXR Reader against a Constant image graph,
verifies alpha, transforms/Axis, native roughness/metallic, nested Scene and
TransformGeo bindings, imported USD domes, image animation, float CPU/Vulkan
color/depth and save/reload of the new Light input and controls. It also covers
the first Reader's lazy project-format adoption: Render3D retains the format
that defined the render request while fetching its textures.

Create, prepare and open the example with::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings','--interpreter','tools/create-scene3d-environment-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DEnvironmentDemoGuiSmoke
    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-environment-demo/environment.ntp')

The 640x360 scene compares SmoothGold (roughness 0.08), RoughGold (0.55) and
BluePlastic (metallic 0, roughness 0.45) using smooth polygon USD spheres,
native PBRMaterial nodes and a generated studio EXR with radiance above 1.
Select **3D** and **Camera** in EnvironmentViewer, then scrub frames 1..25 to
see EnvironmentLight rotate. Its shadow sampling is disabled for quick
reflection inspection; ShadowLight supplies direct ground shadows. Enable
EnvironmentLight **Cast shadows** to inspect environment occlusion. Edit the
material roughness or replace StudioHDR to compare reflection behavior. The
directory includes color PNGs for frames 1/13/25 and a depth preview. The
generator uses the existing Constant/Reader/writer plugins and OIIO tool.

USD validation and example
--------------------------

``NatronUsdScene`` checks references/sublayers/payloads, private session variants,
instance proxies, concave polygons, holes, inherited/per-face materials, indexed
primvars, visibility/purpose, resetXformStack, Z-up centimeters, animated cameras,
light normalization, binary Crate and USDZ import, composition diagnostics and
forced referenced-layer reload, PNG/JPEG/16-bit/grayscale images, straight alpha,
signed tangent normals, independent map UVs and RGBA scalar channels, packed ORM,
degenerate/mirrored UVs, sampler budgets, sRGB/raw, UV animation, material subsets, wrap modes, USDZ images and image reload.
``--gpu N`` compares color and Depth.Z, including texture alpha/wrap/UV transforms, at three
source times on the selected Vulkan adapter with validation enabled.

Run the graph and Viewer checks with::

    .\tools\run-windows-qt6.ps1 -Program Scene3DUsdSmoke
    .\tools\run-windows-qt6.ps1 -Program Scene3DUsdDemoGuiSmoke

Set ``NATRON_VULKAN_DISABLED=1`` to exercise the CPU backend. The graph check
covers typed sockets, retiming, variants, Axis, camera offsets/focal multiplier,
PBR wrapped around Project3D, subtree filtering, TransformGeo, float CPU/Vulkan
color/depth and project roundtrip. The GUI check verifies stage and camera
Gizmo movement/Undo, framing, timeline frames and the saved Viewer layout.

To generate and prepare the USD example::

    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings','--interpreter','tools/create-scene3d-usd-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DUsdDemoGuiSmoke
    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-usd-demo/usd.ntp')

The folder includes the USD root and referenced asset, frames 1/13/25, a depth
preview, checker/normal/packed ORM textures and an editable ``usd.ntp``.
``pbr-maps.usda`` layers the three maps over ``textured.usda``. On the left panel,
the normal texture produces waves, its left/right halves use roughness 0.22/0.72,
and its top/bottom halves use metallic 0/1. The right panel retains the gold
variant. The example also demonstrates animated UVs, mesh/camera transforms and
imported lighting.

USD / hdNVP backend
-------------------

Render3D now offers **USD / hdNVP** (choice identifier ``hdNVP``), using a
persistent OpenUSD/Hydra host and the standalone Ditto render delegate from
the local hdNVP project. The host uses DX12/DXR. Its USD/runtime DLLs are kept
in a separate process because Natron and hdNVP use different compiler/SDK
builds. **Auto** continues to select the existing Vulkan/CPU backend.

Build and run the host in separate PowerShell invocations from the Natron root::

    .\tools\build-hdnvp-host.ps1
    .\tools\build-windows-qt6.ps1 -SkipDependencies
    .\tools\run-windows-qt6.ps1 -Program NatronUsdSceneBridge -ProgramArguments @('--hdnvp')

The builder defaults to the sibling ``../houdini/hdNVP`` project, whose
standalone Release plugin must already be built. Set ``NATRON_HDNVP_ROOT``
or pass ``-HdNvpRoot`` to use another checkout. ``NATRON_HDNVP_HOST`` can
override the worker executable. Close processes using the host before
rebuilding its runtime.

For ReadUSD, this backend composes the original stage as a sublayer beneath
a private Natron layer. References, instances, MaterialX/PreviewSurface
networks, texture paths and USD lights are evaluated by hdNVP. Natron does
not extract its triangles or build its CPU BVH. Source-info mesh/light
counts read metadata without evaluating geometry. Native Card, mesh, PBR
maps and Light nodes are authored as standard USD prims; native mesh face
materials use material-binding subsets. Float TIFF assets preserve HDR and
signed data maps. Beauty, camera-linear Depth.Z, Albedo, world-space N and P,
and integer picking buffers are returned together. Depth and P use Y-up meters;
empty depth and utility pixels are zero. N is the shading normal, including
normal maps. P reconstructs the primary surface from depth at the pixel center;
it is not an averaged position across mixed foreground/background samples.

The worker retains its delegate, Stage, render index and GPU resources
between camera/material/frame requests. USD edits are reloaded with Sdf
diffs and delivered through ``ApplyPendingUpdates``. A dedicated IPC thread
owns QProcess even when Natron changes render-pool threads. Cancellation is
cooperative through shared memory; an unresponsive host is restarted after
a bounded timeout. Camera opinions use a separate layer so camera movement
does not rewrite or reload the geometry layer. Unchanged native geometry and
material resources are retained.

**Path tracing samples** and **Path tracing bounces** control convergence.
Draft/3D Viewer requests cap samples at four. Native antialiasing does not
control this path tracer. The 3D Viewer requests completed frames asynchronously,
retains the previous image while rendering and rejects obsolete preview replies.
Interactive requests can publish intermediate samples while converging.
Writer requests wait for the completed frame. On Windows, frames up to 512 MiB
transfer through a revisioned shared-memory buffer, with files as the larger
frame fallback. This still includes GPU readback and CPU image conversion.
Beauty and utility requests reuse the same completed frame. GPU image sharing
remains future work. The first
frame includes texture loading and GPU shader/pipeline creation, so report
it separately from subsequent camera updates.

**Denoiser** controls the hdNVP color result. **Auto** uses DLSS Ray
Reconstruction for 3D/draft previews when available, with OIDN as fallback;
completed renders use Open Image Denoise explicitly. hdNVP's own batch
``auto`` means no denoising, so the host maps Natron's final Auto to ``oidn``.
**OIDN** filters the converged scene-linear RGB with albedo/normal guides and
preserves alpha, depth and picking buffers. **DLSS Ray Reconstruction** uses
an interactive delegate at native resolution even for a completed request;
it requires a supported NVIDIA GPU and at least 32x32 pixels. **Off** returns
the unfiltered path traced result. Denoiser edits reach the persistent
delegate on every request; switching between batch and interactive modes
recreates the delegate because hdNVP fixes renderMode at construction.
Low sample counts can still lose fine detail after denoising. Host reports
include the selected mode, active denoiser and denoise timings.

ReadUSD retains original materials and now supports **Shot layers** and
**Asset reference** composition. Complete shot layers preserve root paths;
multiple shot inputs intentionally combine opinions in Scene input order
(input 0 is strongest) and require matching units, up-axis, time and payload
policy. Asset references use separate stable namespaces and support ReadUSD/
TransformGeo transforms and independent source sample times. The selected
Prim subtree, or defaultPrim when blank, must include its material/relationship
dependencies. Use Shot layers for a multi-root shot without a common asset
root. Arbitrary whole-shot namespace normalization, opacity/material overrides
and native mesh vertex-color gradients still need adapters. These cases report
an error on this backend. Asset assembly currently rejects nested
``resetXformStack`` operations; use Shot layers to retain those source-space
semantics. A referenced asset root's reset is cleared in the private layer so
the assembly transform applies, while its original transform operations remain.
Imported camera film offsets and focal multipliers are evaluated. Depth of
field is not yet exposed by the Natron camera representation. Imported prim
selection, GPU pixel picking and bounds framing are available through Stage
Outline, including instance-proxy paths. USD edit nodes author per-prim
opinions. Direct imported-prim Gizmos remain future work.
Native geometry and render camera Gizmos remain editable.

.. _maint-3d-stage-outline:

UsdStage composition and Stage Outline
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``Gpu/UsdStageValue`` publishes a branch-private composed UsdStage with an
immutable generated opinion layer, source composition arcs, revision and
sampling time. The bridge exports that assembly for Hydra and supplies the
same composition to Outline inspection. Source files remain read-only; new
published values do not change earlier values' private opinions. Generated
native prim paths use a hidden persistent node UUID, independent of the node's
label and its position in Scene inputs. TransformGeo branches have distinct
instance path identities. Saving an assembly retains references and sublayers,
rather than flattening imported mesh data.

Open **Outline** in the Viewer toolbar, or choose **Stage Outline here** from
a pane's menu. Stage Outline is a single native application panel per project
window: drag its tab between panes, split a pane or float its tab using the
existing workspace controls. Reopening Outline focuses the existing tab and
retains its placement. The Viewer button is available in both 2D and 3D.

**Follow active Viewer** inspects the Render3D branch upstream of the active
Viewer. Select a Render3D in the source menu to pin that branch while switching
Viewers. The pin uses the persistent node identity and survives renames. The
current timeline frame and active Viewer's view select the sample. A deleted
source is reported as missing rather than silently switching to another node.
Panel placement and the pinned source are saved in the project; enable
**Load workspace embedded within projects** to restore both on opening it.
Pane placement also participates in Natron's layout export/import.

Stage composition is independent of the visible 3D preview. The panel can
inspect and update a stage while the Viewer displays a 2D image, without
starting an hdNVP preview. It uses the existing bridge exporter and immutable
UsdSceneValue; source files stay read-only. Composition runs asynchronously
after the native scene and texture snapshot has been captured, and replies
for superseded source/frame/view requests are discarded. Use BindMaterial for
imported material overrides on this backend.

The panel shows the composed prim hierarchy. Expand branches to fetch children
asynchronously in pages of at
most 256; double-click **More prims** for the next page. Filtering applies to
fetched rows. Expanding/searching does not load payloads; use ReadUSD's existing
payload policy. Instance-proxy children can be inspected as read-only entries.

Select one or more rows to inspect prim properties, bindings, contributing
layers, variants and API schemas. The 3D Viewer highlights composed world
bounds, and **Frame selection** (or F in the Viewer) frames those bounds.
The panel's button switches a matching Viewer to 3D if needed; it is disabled
when no Viewer is connected to the selected source. Node ownership selection
also works during 2D inspection. Selection is not transferred between unrelated
stage branches.
Selecting a child reveals its owning graph node while keeping the child prim
selection. Existing native Card/ReadGeo/Light selection also connects to the
Outline. Camera-only Viewer changes retain expanded rows and selected paths;
scene/time changes use revision-tagged replies and restore available paths.
The property view remains read-only. With a Camera selected, **Look through**
sets the inspection camera; **Render camera** separately sets Render3D's
``usdCamera`` path, with Undo/Redo. A stage camera can replace the Camera input.
Inspection cameras follow stage animation while the panel is open.

**Edit** inserts USDTransform, USDVisibility, BindMaterial or USDCompositing
on the inspected Render3D scene input. ``primPaths`` lists exact absolute
composed paths. These nodes and their parameters use normal Undo/Redo and
project serialization; source layers remain read-only. USDTransform is a
world-space delta in Y-up meters and applies once to a parent/child selection.
An instance-proxy override privately makes that instance editable, preserving
other instances. BindMaterial accepts PBRMaterial, Project3D or an image;
ordinary imported textures use ``st``. Arbitrary rename/reparent is future work.

USDCompositing supports Mesh holdout and separate camera, shadow and indirect
ray visibility. Reflection visibility currently governs all indirect rays,
including diffuse bounces. Holdout cuts primary beauty/alpha but retains depth,
picking and secondary-ray participation. This requires the matching hdNVP
renderer changes, not just the Natron schema resources. Curves, volumes,
catchers, UV, motion vectors, motion blur, DOF and Cryptomatte remain subsequent
delivery stages.

Regression coverage is provided by ``UsdStageComposition`` and the real GUI
smoke, including 2D updates without a preview, active-Viewer following, source
pinning, moving/floating/closing/reopening the panel and restoring a floating
layout with its pinned UUID. It also covers framing with Outline and Viewer in
the same pane, and checks that a closed Outline does not restore an old prim
selection when the Viewer reloads its scene. The smoke creates and saves a portable test scene at
``build/scene3d-stage-outline/outline.ntp``:

.. code-block:: powershell

   .\tools\run-windows-qt6.ps1 -Program CTest -ProgramArguments @('-R', 'UsdStageComposition')
   .\tools\run-windows-qt6.ps1 -Program Scene3DStageOutlineGuiSmoke -GuiTimeoutSeconds 180
   .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-stage-outline/outline.ntp')

For the local large test scene, set ``NATRON_STAGE_OUTLINE_SCENE`` to
``D:/test_0001.usd`` before the GUI smoke. Results and its project are written
to ``build/scene3d-stage-outline-user``; the source and its dependencies are
not edited. The GUI smoke verifies that Outline is available while GPU
rendering is still in progress.

``Gpu/usd/natron/resources`` registers codeless NatronCompositingAPI and
NatronProjectionAPI schemas without changing standard prim types. The former
preserves unlit intent alongside the emitted PreviewSurface network. The
projection schema records camera, timing, aspect, image and world-to-camera
data. Project3D emits a MaterialX graph evaluated at each ray hit by hdNVP,
including perspective division, near/far clipping and data-window UV mapping.
It does not depend on interpolated vertex projection. Images retain alpha,
format/PAR and crop semantics; outside the projector is transparent.

Run the interaction/EXR fixture and the expanded GPU bridge regression with::

    .\tools\run-windows-qt6.ps1 -Program NatronUsdSceneBridge -ProgramArguments @('--hdnvp')
    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings','--interpreter','tools/scene3d-usd-interaction-smoke.py')
    $env:NATRON_STAGE_INTERACTION_TEST = '1'
    .\tools\run-windows-qt6.ps1 -Program Scene3DStageOutlineGuiSmoke -GuiTimeoutSeconds 240
    Remove-Item Env:NATRON_STAGE_INTERACTION_TEST

The EXR fixture saves ``build/scene3d-usd-interaction/interaction.ntp`` and
float beauty/utility outputs. The GUI check exercises GPU instance picking,
independent camera actions and USD edit-node insertion/Undo/project reload.

For the supplied ``D:/test_0001.usd`` scene::

    .\tools\run-windows-qt6.ps1 -Program NatronUsdSceneBridge -ProgramArguments @('--scene','D:/test_0001.usd','build/hdnvp-test-0001')
    .\tools\run-windows-qt6.ps1 -Program NatronUsdSceneBridge -ProgramArguments @('--denoiser-scene','D:/test_0001.usd','build/hdnvp-denoiser-test')
    .\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--no-settings','--interpreter','tools/create-scene3d-hdnvp-demo.py')
    .\tools\run-windows-qt6.ps1 -Program Scene3DHdNvpDemoGuiSmoke
    .\tools\run-windows-qt6.ps1 -Program Natron -ProgramArguments @('build/scene3d-hdnvp-demo/hdnvp.ntp')

The generator uses ``NATRON_HDNVP_TEST_SCENE`` to override the scene path and
selects ``/cameras/camera1``. The GUI check saves HdNvpViewer in 3D/Camera mode,
checks camera edits and Gizmos, and saves a Viewer screenshot. It also switches
between 3D and 2D during active previews, verifies arrival of new 2D frames and
checks that cancellation leaves no persistent Render3D error. Abort predicates
run on the calling render thread; the host thread receives an atomic flag.
The standalone
scene check writes linear float RGBA, sRGB previews and ``result.json`` with
initial load, initial render, camera update and renderer GPU timings. In the
initial RTX 5080 measurement (640x360, 8 samples, 4 bounces), stage metadata
took 206 ms, first rendering 26.2 s (including a 23.4 s MaterialX pipeline
creation), and a camera update 88 ms including IPC/readback. Eight-sample GPU
ray dispatch was 34.2 ms after the update. These are measurements of that
scene and run, not general frame-rate guarantees.
