.. _maint-3d-design:

USD 3D compositing design
=========================

Status: proposed implementation design, 2026-10-13. The implementation baseline
is commit ``f9f020dfc``. This document describes target behavior; it does not
claim that the proposed nodes, schemas or renderer features already exist.
See :ref:`maint-3d-compositing` for the current implementation and limitations.
The composed-stage foundation and initial Stage Outline browsing/selection are
now implemented; :ref:`maint-3d-stage-outline` describes their present scope.

Design direction
----------------

Make a composed UsdStage the common scene representation behind Natron's
existing node graph. Implement scene creation, assembly and edits by authoring
prims, properties, relationships and composition arcs into private USD layers.
Use Hydra/hdNVP for both image-based compositing and lit utility rendering.
Keep familiar Card, Camera, Project3D, Scene and Render3D workflows. Image
inputs come from Natron's normal 2D graph and rendered results return to that
graph as scene-linear, premultiplied images and explicitly defined data planes.

Stage Outline presents the actual composed prim hierarchy used for rendering.
It shares selection and inspection with the 3D Viewer and node graph. Scene
assembly, Outline inspection, rendering and export must agree on this stage.

The design follows the user's priorities: image placement in 3D, camera
projection and parallax first; preserve production USD assets and use the
existing GPU path tracer. Extend standard USD with Natron schemas where comp
semantics require them, and implement those semantics in renderer adapters.
A schema declaration alone is never treated as working renderer support.

Nuke workflows informing the design
-----------------------------------

Foundry's `current new 3D overview
<https://learn.foundry.com/nuke/current/content/comp_environment/usd-3d-comp/usd-3d-comp.html>`_
identifies cleanup projections, matte paintings and background details as
comp workflows. These motivate the following Natron acceptance scenarios.

.. list-table:: Workflow requirements
   :header-rows: 1
   :widths: 22 38 40

   * - Workflow
     - Artist task
     - Natron requirement
   * - 2.5D matte painting
     - Arrange cut-out images and simple surfaces; animate a shot camera.
     - Cards, alpha edges, filtering, depth ordering, camera framing and
       interactive parallax without changing painted colors through lighting.
   * - Cleanup and set replacement
     - Project a repaired plate onto proxy geometry from a chosen frame.
     - Separate projection/render cameras, independent camera/image holds,
       projection masks, near/far clipping and controlled occlusion.
   * - Projections on moving surfaces
     - Keep paint attached to a translating or deforming surface.
     - Reference-surface coordinates, explicit topology validity, and
       independent animation of the projected image.
   * - Background set dressing
     - Add storefronts, signs, screens or background elements and reuse them
       across shot cameras.
     - Multiple sources, reusable groups, camera switching, instancing,
       object selection and non-destructive overrides.
   * - Additional interaction passes
     - Place a fire/explosion image in a scene and generate interactions.
     - Camera/reflection/shadow visibility, holdouts, catcher roles, PBR,
       environment lights and separately usable render passes.
   * - Comp data and relighting
     - Generate masks, depth and surface information for later 2D operations.
     - Coverage-aware masks and defined Z, P, N, UV and motion conventions;
       stable object identities and an eventual relighting node.

For a production example, Territory Studio added window displays, theatre
signs and billboards inside Nuke for *Mank*, and reused that work with different
shot cameras. The requirement we derive is reusable 3D dressing with reliable
camera handover, rather than a separate graph per camera. See
`Foundry's Mank case study
<https://www.foundry.com/insights/film-tv/1930s-mank-nuke>`_.

The *Thor* account describes matte-paint mapping in Nuke/Maya and problems with
projection seams, stretching and occlusion. It motivates explicit projection
diagnostics and multiple masked projections. See `Foundry's Thor case study
<https://www.foundry.com/insights/film-tv/thor>`_.

Foundry's `new-system rendering guide
<https://learn.foundry.com/nuke/current/content/comp_environment/usd-3d-comp/render-scene.html>`_
describes reflection/refraction passes that ground newly comped elements.
This motivates utility-pass controls around hdNVP's PBR rendering.
These are design inferences from the documented examples, not claims of
file or node compatibility with Nuke.

Current capability boundary
---------------------------

Native nodes already provide cards, transforms, cameras, FBX animation,
projection, PBR maps and lights through the older CPU/Vulkan scene path.
The USD/hdNVP bridge preserves one complete imported USD stage, including
references and instances, and renders native geometry/material/light additions.
It returns float color/depth and integer picking buffers, with functioning
OIDN/DLSS choices. These paths currently have different coverage.

The following remain implementation work:

* USD/hdNVP Project3D, arbitrary multi-root asset normalization and prim-scoped
  edit-node overrides beyond the current asset assembly transforms.
* Imported-object pixel picking and Outline camera/edit actions. The dockable
  Outline panel supports source following/pinning, prim inspection/selection,
  bounds framing and workspace restoration.
* Progressive sample presentation and incremental texture transport. Completed
  3D preview frames are now requested asynchronously.
* Full imported camera optics and consistent render/projection framing.
* Comp visibility, holdouts/catchers and additional public AOVs.

hdNVP's current ``render::AovContent`` supports color, depth, integer IDs,
albedo and world-space shading normals. Natron currently exposes only the
first four through its host, and only color/depth as image outputs. Position,
UV, motion, lighting-component passes and Cryptomatte require renderer or
bridge work; requesting an unsupported pass must not silently return zeros.

Scene representation and node evaluation
----------------------------------------

Introduce a ``UsdSceneValue`` in place of triangle snapshots as the main 3D
value passed between nodes. It describes the layer-composition recipe, revision,
selected prims, time mappings and material image dependencies that produce
one composed UsdStage for an evaluated scene branch. Geometry, Camera,
Material and Light sockets remain typed views with prim-path selections.
The node graph and external assets are the persisted source of truth;
generated USD layers and stages are derived caches. A stage handle is owned
by the USD evaluation thread/process, not passed across the host ABI or used
concurrently for UI inspection and mutation.

Each modifier authors its own private opinion layer. Published upstream layers
are immutable; changes use new layer versions so sibling branches keep their
previous results. The evaluation owner composes the selected branch's layer
stack and applies the node's opinions through an explicit edit target. A
Stage Outline edit creates or edits a graph node so save/load and undo
reproduce the result. Imported layers and files remain read-only; persistent
behavior must not depend on hidden session edits outside the graph.

Use persistent node UUIDs for generated prim paths, replacing array-position
paths such as ``Object0``. Keep artist labels independent of stable identity.
Maintain a mapping from composed prim path and instance context to source prim,
owning node, source layer and private override layer. A Scene merge preserves
common upstream provenance once and defines input strength/order explicitly;
divergent opinions on the same prim must be inspectable. Distinct imported
assets receive distinct namespaces rather than colliding on equal root names.

Use standard prims and APIs before adding custom schemas:

.. list-table:: Node operations expressed in USD
   :header-rows: 1
   :widths: 25 45 30

   * - Node or operation
     - USD representation
     - Authored behavior
   * - Card / ReadGeo
     - UsdGeomMesh under UsdGeomXform; primvars and GeomSubsets as needed.
     - Geometry, animation, image coordinates and material relationships.
   * - Axis / TransformGeo
     - UsdGeomXform and selected prims' xform operations.
     - Branch-local transform opinions with explicit parent/world space.
   * - ReadUSD / Scene
     - References, payloads, sublayers and private assembly layers.
     - Source assembly, namespace, variant selection and time mapping.
   * - Camera / Light
     - UsdGeomCamera and appropriate UsdLux light prims.
     - Optics, transforms and supported lighting attributes.
   * - PBRMaterial / BindMaterial
     - UsdShadeMaterial/Shader networks and MaterialBindingAPI.
     - PBR maps and bindings on selected prims or subsets.
   * - Selection sets
     - UsdCollectionAPI with explicit path/instance selection semantics.
     - Reusable targets for supported edit and binding operations.
   * - Project3D / comp roles
     - UsdShade network plus NatronProjectionAPI/NatronCompositingAPI.
     - Projection-camera relationships, timing and comp-specific intent.
   * - Render3D
     - UsdRenderSettings, Product and Var prims.
     - Render camera, supported settings and output requests.

Keep hierarchical transform inheritance and ``resetXformStack`` behavior
explicit when inserting assembly transforms. Instance-root overrides should
preserve prototype geometry. Descendants that are instance proxies are not
ordinary writable prims; operations that require changing a prototype or
breaking instancing need a defined graph operation and visible consequence.
Point-instancer element selection also carries instance context, not just a
prim path.

Distinguish two USD import/assembly operations:

* **Shot layer composition** stacks intentional opinions in the same namespace.
  Retain the existing complete-shot sublayer import, including separate
  geometry, materials, lights, cameras and render-setting roots.
* **Asset assembly** references or payloads a dependency-complete source subtree
  under a stable namespace, for example ``/World/Inputs/n_<nodeUUID>``. Repeated
  assets can share references/prototypes while instance-root opinions differ.

A whole-shot file without a dependency-complete common root needs a private
wrapper/normalization plan before it can be assembled as an independent asset.
Referencing each top-level root separately must not lose cross-root material
bindings, camera relationships, collections or internal references. Preserve
relative asset resolution, variants and payloads in the plan. Do not flatten
mesh/material data merely to obtain one root or rely on a missing defaultPrim.
Validate this plan on the pinned OpenUSD SDK before enabling arbitrary
multi-shot assembly; unsupported layouts require a clear import diagnostic.

OpenUSD's `namespace editing guide
<https://openusd.org/release/user_guides/namespace_editing.html>`_ describes
relationship repair and relocates, but namespace edits may affect several
layers and depend on currently populated composition. An edit target alone
does not guarantee source isolation. Any normalization using namespace edits
must operate on controlled private layer copies/wrappers and verify the
complete dependency mapping, including unloaded payloads and variant branches.
Initial Outline operations avoid arbitrary rename/reparent of imported prims.

Preserve source units, up-axis, FPS, references, variants and payloads as source
metadata/dependencies. Author explicit assembly-space and time conversions;
referencing a file does not convert its units/up-axis automatically. New native
node coordinates and public dimensional AOVs remain right-handed Y-up meters.
Camera dimensional attributes also need defined conversion; a parent transform
alone does not settle every camera value. Keep legacy stage framing compatible.
Unknown USD attributes remain in the source layers.

UsdImaging/Hydra consumes the resulting UsdStage. Scene-index adapters may
derive renderer data for projection or other custom schemas, but scene assembly
and persistent edit semantics must already be present in the USD composition.
The earlier multi-stage Hydra prefix/merge proposal is superseded by this
single-composed-stage contract. A renderer adapter implements shader/ray
behavior declared by prim properties; writing those properties alone does not
implement projection, holdouts or lighting in the GPU renderer.

Export saves the private assembly/opinion layers and rewrites their generated
layer/asset identifiers into an explicit package with dependency policy.
References and source layers are retained where requested. ``UsdStage::Export``
produces a flattened layer, so it is a separate explicit flatten-export option.
Round-trip tests compare composed paths, bindings, instances, camera values and
rendered results. Viewing a live stage does not automatically make its anonymous
layers or generated image dependencies portable.

Existing ReadGeo keeps ufbx for FBX ingestion. Convert evaluated ufbx geometry
and cameras into the same USD value, retaining hierarchy/instances and updating
only changing transforms/points. It must not build the older CPU-renderer BVH
for a USD/hdNVP request. Existing triangle evaluation remains a compatibility
adapter while parity is being established.

Stage Outline and prim editing
------------------------------

Add a dockable **Stage Outline** panel showing the composed stage for the active
Scene/Render3D branch at the current frame and view. The panel header identifies
the branch and evaluated revision; allow pinning a branch for inspection.
Display the actual prim hierarchy, including imported roots and Natron-generated
prims, rather than reconstructing a tree from graph nodes or render triangles.
One ReadUSD node may own many Outline entries. Changing the inspected branch
does not silently change Render3D's camera or input connection.

The first version provides:

* A tree with prim name/type, effective visibility and instance/payload status.
  Tooltips/details show the full path and source/owning node. Label visibility
  as inherited/authored when relevant; a visible child opinion cannot override
  an invisible ancestor. Provide search by prim path/name and type filters for
  geometry, cameras, lights and materials while retaining ancestor context.
* Selection synchronized between Outline and 3D Viewer, with a separate prim
  selection from graph-node selection. Selecting an imported child can reveal
  its owning ReadUSD node without replacing that child selection by all of the
  node's geometry. Multi-selection and frame-selection use composed bounds.
* A Prim Properties inspector with evaluated transforms, camera/light values,
  material bindings, applied APIs, references, variant selections and payload
  state. An optional composition/layer details view explains contributing
  opinions and the intended override layer. It does not infer every resolved
  value solely from the strongest prim spec.
* Explicit actions to look through a camera or make it the render camera.
  Ordinary selection does neither. Transform, material binding, visibility
  and variant edits create/update appropriate graph nodes with undo/redo.
  Define an insertion point on the inspected branch before making such an edit;
  a pinned upstream branch must not redirect an edit to a different output.

Selection and inspection are available before path-tracing convergence and
do not request a beauty render just to expand a row. A Viewer-only isolate mode
is temporary inspection state; persistent render visibility is a graph-authored
USD opinion. Payload loading is explicit runtime working-set state, with any
project-persisted load policy recorded on its graph node. Expanding or searching
the tree does not silently load payloads. Distinguish ``visibility``, ``active``,
payload load state and renderer-specific ray visibility in the inspector.

Use a lazy Qt item model fed by revision-tagged host/evaluation snapshots.
Query direct children and selected-prim details in bounded requests, not a full
stage traversal on every UI update. Include inactive/unloaded prims when
requested for inspection, without inventing unavailable descendants. Show
instance roots normally; inspect instance-proxy children lazily as read-only
and expose prototypes in an optional diagnostic view. Prototype runtime paths
are not persistent selection identities. Search reports its populated-stage
scope rather than suggesting that it inspected unloaded payload contents.

USD notices invalidate affected rows/properties/bounds. Requests and replies
carry branch, stage revision and time/view so stale replies cannot update a
different Outline. Runtime picking IDs resolve through a matching composed
path/instance table; preserve selected stable paths across camera-only updates
and visibly clear unavailable selections after composition changes. UI models
hold data snapshots, never mutable UsdPrim handles from the render thread.
Extend this read-only browsing/selection foundation with edit actions once
private layers and graph undo are working.

Acceptance scenes cover two imported assets with identical source root names,
cross-root material bindings, multiple cameras, native cards mixed with USD,
inactive/unloaded prims and large instanced hierarchies. Verify Outline/Viewer
selection agreement, camera actions, bounds, filters, branch pinning, undo/save/
reload, and stale-reply rejection during rapid frame/input changes. Scene
inspection must not rebuild a CPU BVH or duplicate every instance's mesh.

Projection and material semantics
---------------------------------

Keep Project3D as an image-and-projection-camera material operation, and add
an explicit BindMaterial operation for applying a material to selected USD
prims/subsets. Preserve the current convenient material sockets on native
geometry. Add UVProject as a geometry operation for reference-surface mapping;
it must not overwrite a mesh's original ``st`` unless requested.

Nuke distinguishes a projection material shader from projected UV generation.
Its guide also calls out distortion on large polygons with vertex-only UV
projection. See `Foundry's projection guide
<https://learn.foundry.com/nuke/current/content/comp_environment/usd-3d-comp/projections.html>`_.
Natron should evaluate world-space camera projection at the ray hit in hdNVP,
including reflected hits, rather than approximate it with ordinary two-channel
UVs on a coarse mesh. For a reference-surface projection, retain homogeneous
projection coordinates/reference surface data and use perspective-correct
evaluation; do not bake already-divided UVs and assume linear interpolation
will preserve a perspective image on a large triangle.

Represent these independent decisions:

.. list-table:: Projection timing
   :header-rows: 1
   :widths: 28 72

   * - Control
     - Meaning
   * - Projection camera: live/reference
     - Sample the projection camera now, or hold its complete optics/transform
       at a reference time. The render camera continues independently.
   * - Image: live/hold
     - Sample the image sequence now, or hold a chosen image frame. Camera
       locking does not implicitly stop a video texture.
   * - Attachment: world/reference surface
     - World projection evaluates the current hit in world space. Reference
       surface projection retains coordinates assigned at a reference pose,
       so the result follows rigid motion or supported deformation.
   * - Geometry time mapping
     - Retime the USD/FBX branch independently from camera/image timing while
       preserving a defined correspondence to the reference surface.

Frozen world projection is not sticky projection: geometry moving through a
fixed projector changes where the image lands. Sticky projection initially
requires stable topology and reference vertex/face correspondence. Invalidate
and explain an incompatible topology change instead of silently stretching
paint onto unrelated vertices. Convert project frames to each source's time
codes explicitly; distinguish held authored poses from subframe sampling.

Preserve the existing ``freeze`` knob when loading old projects: it maps to
reference camera plus held image, with world attachment. Do not reinterpret
old projects as sticky projections.

The projection contract includes input display/data windows, pixel aspect,
overscan, crop/extend/transparent outside behavior, front/back faces and near/
far clipping. Add optional self/world occlusion with a tolerance in meters.
Default occlusion remains off for predictable paint projection. Foundry's
`Project3DShader controls
<https://learn.foundry.com/nuke/17.0v1/content/reference_guide/3dnew_nodes/project3dshader.html>`_
provide a useful reference for these choices. Later add ordered projection
layers with image masks, opacity and explicit blending, instead of requiring
duplicate geometry to blend several cameras.

Extend NatronProjectionAPI's current data contract with independent timing,
attachment, clipping, source-window and occlusion information. A renderer
adapter resolves camera relationships and lowers projection materials to
hdNVP GPU evaluation. Shader identifiers/parameters form a versioned contract
between Natron and hdNVP. An exported projection requiring that adapter must
carry its dependencies and version; other renderers may not reproduce it.

Rendering for compositing and PBR
---------------------------------

Expose two render purposes on the same USD/Hydra backend:

* **Image compositing** evaluates unlit images, projected paint, coverage and
  depth with controlled antialiasing/filtering. Painted colors do not depend
  on lights, tone mapping or exposure. Denoising defaults off for this purpose;
  it must preserve thin lines, text and transparent edges. GPU primary-ray
  tracing and ordered transparent-hit compositing still run through hdNVP.
* **Lit utility rendering** evaluates PBR and requested interactions/passes.
  DLSS supports interactive previews and OIDN supports final denoising; retain
  raw beauty alongside filtered beauty when requested. Final sample budgets,
  seeds and history policy must be reproducible across render order.

This is a target purpose distinction, not two scene models or two separately
maintained feature sets. Existing hdNVP nodes keep their current behavior when
loading projects. Add purpose defaults for new nodes only after compatibility
tests, and switch the existing Renderer Auto default to USD/hdNVP only when
the documented parity criteria below pass.

Extend NatronCompositingAPI for unlit behavior, holdout/catcher roles and
ray-specific visibility where standard USD cannot express the comp intent.
Implement corresponding hdNVP material/ray behavior. Separate visibility to
the render camera, reflections/refractions, illumination and shadow rays.
Unlit image cards must not automatically become unwanted light sources merely
because their color was encoded as PreviewSurface emission.

Keep background plates separate from environment illumination: the same image
may be connected to both intentionally, but a background connection does not
create a dome light. Camera-invisible geometry can still cast shadows or
appear in reflections. Define holdout coverage, catcher output and background
Merge behavior with reference scenes before assigning public channel names.

Keep standard PreviewSurface and MaterialX networks for PBR, including data
maps with explicit channel/normal conventions. Preserve imported bindings;
overrides operate on selected prims/subsets. Texture, displacement and material
features are advertised through host capabilities, not inferred from the fact
that the source file is USD. Unsupported features produce actionable errors.

Color, cameras and AOV contracts
--------------------------------

Natron image inputs are evaluated through the normal render engine at explicit
times/views and in the project's OCIO working space. Carry color/data intent,
premultiplication, image windows and UV transforms with each texture request.
Normal/roughness/metallic/mask maps are data. Apply the Viewer display transform
once, after linear rendering. Check imported USD texture/color metadata against
the working space; loading USD does not guarantee color equivalence.

Pass complete USD camera optics through the bridge: apertures/offsets, framing,
near/far, focal length, and later shutter/DOF controls. ReadUSDCamera overrides
must modify explicit fields without discarding other imported camera values.
Use one framing implementation for projection, rendering, Viewer overlays and
picking. Start shot integration with undistorted plates and external STMap/
lens-distortion workflows; add native lens models as a separate capability.

Use standard `UsdRenderSettings, Product and Var
<https://openusd.org/release/api/usd_render_page_front.html>`_ descriptions for
render settings and requested outputs, lowering them to supported Hydra AOVs.

.. list-table:: Output contract and implementation order
   :header-rows: 1
   :widths: 25 50 25

   * - Output
     - Contract
     - Implementation order
   * - Beauty / raw beauty
     - Scene-linear premultiplied RGBA; define transparent-edge behavior.
       Denoising affects eligible color, never data planes.
     - Current; extend for render purpose/raw retention.
   * - Depth.Z
     - Positive camera-planar -Z in meters; empty 0. Define mixed coverage and
       matte behavior; optional distance is a different plane.
     - Current; preserve and document sampling semantics.
   * - Picking / object masks
     - Internal integer IDs plus revision-matched path/instance mapping.
       Public masks require sample coverage, not a nearest-ID color lookup.
     - Scene editing and holdout milestone.
   * - Albedo / N
     - Explicit raw albedo and world-space shading normal; later distinguish
       geometric/camera normals. Preserve vector/data treatment at edges.
     - Bridge existing renderer data after semantic tests.
   * - P / UV
     - World position in meters; named source/projected UVs. No color transform
       or denoising; explicit miss values and filtering policy.
     - Renderer extension.
   * - Motion
     - Define source/destination time, image-space units, direction, origin
       and shutter. Verify downstream 2D motion-blur behavior.
     - Motion milestone; renderer extension.
   * - Shadow / reflection / transmission
     - Separately usable interaction outputs and compositing equations.
       Beauty recombination tests precede public pass promises.
     - Comp-visibility and lighting milestone.
   * - Cryptomatte
     - Stable names/hashes, multi-sample ID coverage and EXR metadata/manifest.
       Current primId/instanceId buffers do not implement this contract.
     - Dedicated renderer/Writer integration milestone.

For Cryptomatte use the `Psyop specification and reference implementation
<https://github.com/Psyop/Cryptomatte>`_; validate interoperability, including
transparency and motion, rather than relabeling the current picking IDs.

Interactive rendering and update ownership
------------------------------------------

Introduce a RenderSession behind Render3D/Viewer, with immutable requests and
scene, camera, texture and settings revisions. Publish generation-tagged
partial frames from a GPU-owning render thread. The UI never waits for stage
loading, shader creation, rendering or readback. Only the newest preview job
may replace the displayed image; final Writer jobs remain ordered and are
not discarded by the preview's latest-request policy.

Keep the last complete image visible while a new frame prepares, with clear
loading/rendering/error status. Picking uses the same image/camera generation
and a matching identity table. Reject stale picks and stale frame replies.
Stage Outline and 3D selection share prim/instance identity and reveal the
owning graph node without expanding a child pick to its entire imported scene.
Move/rotate/scale, orbit/pan/dolly, frame-selection, projection-frustum display,
image masks and checker/UV diagnostics work independently of convergence.

Replace whole-scene file reload on routine edits with versioned USD layer/
property updates on the evaluation owner. USD change notices drive Outline
invalidation and UsdImaging/Hydra pending updates; the IPC protocol transports
USD edits/dependencies rather than defining a second authoritative scene graph.
Transforms/cameras, material parameters, lights, textures, topology and payloads
have different invalidation scopes. A camera move retains geometry, textures
and pipelines. A frame change updates the relevant time-varying data. Reset or
reproject DLSS history according to actual changes; never leak old history into
a final frame from a different time, camera, topology or projection.

The host must process cancellation without destroying the delegate for every
camera update. Use a reader/control queue and cancellation tokens while all
USD mutation and GPU work remain on their owner thread. Hard process termination
remains timeout/fault recovery. Retain the current process ABI isolation.
Preview and final sessions may need different delegate modes because current
hdNVP fixes batch mode at construction; account for their VRAM cost explicitly.

First replace temporary per-frame image/AOV files with bounded shared-memory
buffers and incremental texture updates. Release content-addressed textures
with cache eviction rather than retaining every frame indefinitely. Then
evaluate DX12/Vulkan GPU image sharing on matching adapters with explicit
resource lifetime and fence ownership. CPU/shared-memory transport remains a
valid fallback; GPU sharing is not a prerequisite for functional projection.

Proposed interaction targets are UI response within 50 ms and warm 640x360
four-sample preview publication within 150 ms on the current RTX 5080 test
system, measured at p95. These are targets, not measured guarantees. Report
initial stage/texture/pipeline preparation separately, and track camera,
texture-sequence and deformation updates plus VRAM retention over time.

Implementation milestones and acceptance scenes
-----------------------------------------------

1. **Composed stage and projection foundation.** Introduce the USD scene value,
   private layers and stable prim identity incrementally. Add Stage Outline
   browsing, source/property inspection and branch-aware prim selection.
   Implement BindMaterial and the hdNVP projection adapter. Test static/frozen-
   camera and live-image projection, crop/PAR/overscan, coarse angled meshes
   and independent render-camera animation. Add asynchronous preview ownership
   before exposing heavy interactive edits.
2. **Scene editing and reference-surface projection.** Extend Stage Outline with
   edit-node actions, bounds/picking, selection sets and
   TransformGeo/material/visibility opinions,
   multiple-source assembly and stable-topology UVProject. Test source/branch
   isolation, references without defaultPrim, cross-root materials, variants,
   payloads, prototype sharing, instance overrides, undo and project reload.
3. **Comp utility passes.** Add holdout and ray visibility first, then catchers
   and public albedo/normal/position/UV outputs. Test a comped image card seen
   in a reflection, camera-invisible occluders, shadow compositing and correct
   alpha/data edges. Implement each pass end-to-end through EXR Writer.
4. **Temporal and delivery tools.** Add motion vectors, camera/rigid motion blur
   and DOF, then supported deformation blur and Cryptomatte. Evaluate shutter
   subframes and random frame order, including denoiser/history resets. Add
   relighting from defined P/N data after the input/output contracts are stable.
5. **Extended workflows.** Prioritize stereo/multiview, spherical projection,
   point instancing/particles or volumes by actual shot needs and renderer
   capabilities. Camera-track interchange precedes a native camera-solving tool.

Maintain small authored/licensed portable fixtures for every acceptance scene.
Use ``D:/test_0001.usd`` locally as a large-reference/MaterialX/HDR/camera stress
scene, not as the only correctness fixture or a redistributed test asset.
Keep its source and referenced assets read-only.

Existing projects must retain their node IDs, knob semantics and renderer
selection. Introduce a versioned migration only after old CPU/Vulkan projection
and USD/hdNVP scenes agree on camera framing, color/alpha, filtering, depth,
time mapping and save/load behavior. Preserve explicit compatibility backends
during migration, with feature detection before rendering and clear failures
when a requested feature is unavailable.

The current implementation uses branch-private composed stages and a dockable
Stage Outline. The interaction slice adds GPU Prim/instance picking,
independent inspection/render camera actions, USDTransform/USDVisibility/
BindMaterial/USDCompositing opinions, ray-hit MaterialX projection, shared
memory with cooperative cancellation, progressive previews and depth/albedo/
normal/position EXR outputs. See ``3d-compositing`` for the exact output and
compatibility contracts. Direct Prim Gizmos, catchers, UV and temporal delivery
passes remain subsequent slices; schema declarations alone do not count as
implemented renderer support.
