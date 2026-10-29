.. _maint-3d-workflow-review:

Nuke workflow research and design assessment
============================================

Research date: 2026-10-29. Primary evidence: Foundry's versioned Nuke 17.1
User Guide and node references. Natron baseline: ``5344d7689``.
This is a documentation-based workflow assessment, not a Nuke runtime test,
solver benchmark or claim about Foundry's unpublished implementation.
The resulting Natron proposal is :ref:`maint-3d-redesign`.

Assessment
-----------

USD-centered scene assembly with native camera tracking and point-driven
placement is a sound product direction. Delivering that workflow requires
separate contracts for image analysis, reconstructed scene data, interactive
scene tools and rendering. A generic USD importer plus a path tracer does not
by itself deliver Nuke-like compositing workflows.

Keep reconstruction, sparse points, alignment, card/proxy placement and
projection in the first complete shot workflow. Follow with dense point
generation and meshing. Revise the initial CameraTracker scene-output choice,
make comp-utility rendering independent of the high-quality renderer, and
specify merge/override behavior before broadening USD editing.

Classic and new 3D have different boundaries
---------------------------------------------

Foundry positions the new USD system around comp tasks such as cleanup
projections, matte paintings and background additions, while retaining Classic
3D. It explicitly distinguishes this from replacing a full DCC. See the
`new 3D overview
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/usd-3d-comp.html>`_.

**Natron implication:** retain familiar artist operations and existing project
behavior. USD should enable asset assembly and non-destructive edits without
making every shot a layer-authoring exercise. This supports a shared USD scene
contract, but does not prove that Nuke uses the proposed SceneService or recipe
DAG internally. Those are Natron engineering decisions.

Tracking, scene creation and alignment
---------------------------------------

The `CameraTracker reference
<https://learn.foundry.com/nuke/17.1/content/reference_guide/3d_nodes/cameratracker.html>`_
documents separate track/solve/refine actions, masks, manual/imported tracks,
camera/lens settings and per-frame errors. Its New 3D option chooses which
system its Create action generates. Analysis is shared across the two systems;
it is not replaced by USD composition.

The `scene creation guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/cameratracker/creating_scenes.html>`_
describes generated camera/point-cloud/scene nodes and linked or baked camera
results. Its Scene+ preset also builds the lens/render setup. The general
tracking guides still use some Classic node names; use the versioned node
reference to identify new-system behavior rather than assuming every named
node is a native USD node.

**Natron implication:** CameraTracker should remain an image-domain analysis
tool. Create Scene can produce a ReconstructionScene and camera selector in
one undo transaction. Use explicit accepted-solve dependencies and linked/baked
results, instead of assuming that CameraTracker's sole output must be a scene.
Reusing the existing 2D tracking adapter is plausible; 3D solve quality and
dependency closure still need a native validation target.

The `alignment guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/cameratracker/adjusting_scene.html>`_
separates ground/axes/origin, manual transforms and distance constraints.
The `constraints guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/cameratracker/creating_contraints.html>`_
uses a measured distance between solved points to define scene scale.

**Natron implication:** AlignScene is justified. Apply one solve-to-world
mapping to cameras, points and attachments. Ground and origin alone do not
calibrate physical scale. Preserve the raw result and its scale status rather
than silently describing arbitrary reconstruction units as measured meters.

Sparse points, dense points and proxy geometry
-----------------------------------------------

The `placement guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/cameratracker/placing_objects_scene.html>`_
selects solved points in the plate and creates geometry at their average
position. Switching between the plate and point cloud helps choose the region.
It does not establish that every such creation performs a robust plane fit.

**Natron implication:** shared plate/3D point selection and Card/Axis creation
are core workflow requirements. Robust plane fitting, stable point IDs and
explicit orientation choices are proposed Natron improvements, not claims
about CameraTracker's internal fitting method. Manual correction against the
plate is needed when landmarks do not adequately describe a surface.

The `GeoGeneratePoints reference
<https://learn.foundry.com/nuke/17.1/content/reference_guide/3dnew_nodes/geogeneratepoints.html>`_
documents a separate dense process taking source images, a solved camera and
an optional mask. It selects keyframes using camera baseline, tracks denser
points, filters by triangulation angle/density, and offers groups and baking.
This is more specific than simply turning a sparse solve into a mesh.

The `point-cloud overview
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/create-point-cloud.html>`_
also distinguishes GeoPoints, GeoGeneratePoints, DepthGenerator,
DepthToPosition and GeoPointsToMesh. Geometry/depth conversion, camera-derived
dense reconstruction and meshing are different operations.

**Natron implication:** evaluate dense correspondence/triangulation first,
reusing solved poses and tracking data. Do not require a full multi-view-depth
fusion engine for the initial dense-cloud feature. Depth-to-points is useful
when depth already exists, but does not recover photographed geometry merely
because a renderer can output Z/P.

The `ModelBuilder reference
<https://learn.foundry.com/nuke/17.1/content/reference_guide/3d_nodes/modelbuilder.html>`_
uses a tracked camera and plate for alignment and can use points/geometry as
reference. It also supports baking geometry and projection setups.

**Natron implication:** simple editable proxies and point-assisted placement
can deliver useful cleanup shots before dense reconstruction or a general
modeling tool. Later multi-frame object/vertex alignment can reuse the
observation/camera contracts; it should not be conflated with camera solving.

Cameras, projection and lens domain
-----------------------------------

The `camera workflow
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/working-with-cameras.html>`_
distinguishes regular Camera nodes from USD GeoCamera prims. Camera can read a
scene camera for cam sockets; GeoScene can inject a camera into USD. Live reads
and snapshots are explicit. USD camera creation alone does not guarantee
compatibility with every existing camera input.

**Natron implication:** retain a SceneCamera adapter for Render3D/Project3D
and complete world-transform/optics conversion. Reading a camera and authoring
a new camera are distinct operations. Avoid accidental duplicate camera prims.
Keep inspection, projection and render camera selection independent.

The `projection guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/projections.html>`_
separates GeoProjectUV, which changes vertex UV attributes, from
Project3DShader, a projection material. Reference-frame UV projection supports
sticky mapping; the guide also warns of tessellation/viewport perspective
limitations on coarse surfaces.

**Natron implication:** preserve separate shader projection and UV/reference-
surface operations. The existing per-hit MaterialX projection is useful for
coarse proxies, but it does not implement sticky deformation projection by
itself. Test preview and output framing rather than assume their material
implementations match.

The `lens workflow
<https://learn.foundry.com/nuke/17.1/content/comp_environment/cameratracker/lens_distortion.html>`_
offers undistorting the plate or distorting rendered additions before merging.
It also supports lens preprocessing outside CameraTracker.

**Natron implication:** define distorted/undistorted image domains, crop,
overscan, calibration and inverse map use in the first shot workflow. A pinhole
MVP is reasonable for explicitly undistorted input; ordinary distorted footage
needs supported preparation/redistortion inside Natron to meet the user's goal.

Assembly, selections and non-destructive overrides
---------------------------------------------------

The `GeoMerge reference
<https://learn.foundry.com/nuke/17.1/content/reference_guide/3dnew_nodes/geomerge.html>`_
documents layer composition, explicit duplication and flattening modes.
Rejoining branches with the same prim paths resolves competing opinions rather
than creating two objects. GeoScene additionally accepts regular axis-based
camera/light inputs.

**Natron implication:** distinguish composition from namespaced assembly and
duplication. Define branch strength and shared-upstream deduplication; include
split/modify/rejoin fixtures. Keep Natron's existing input-strength contract
through migration rather than copying Nuke's A/B ordering onto old projects.
An explicit duplicate/reference action should create another identity.

The `USD authoring guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/usd-concepts.html>`_
provides Set Value and No Change authoring states. The
`GeoBindMaterial reference
<https://learn.foundry.com/nuke/17.1/content/reference_guide/3dnew_nodes/geobindmaterial.html>`_
exposes target masks, binding strength and preview/full/all binding purposes.

**Natron implication:** selective overrides need an explicit no-opinion state.
Reading/displaying a resolved attribute must not author a replacement.
Material binding scope, strength and purpose are distinct controls. Preserve
an existing binding unless the graph requests a change.

The `Scene Graph guide
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/manage-scenegraph.html>`_
documents synchronized selection, type/path picking and temporary Viewer
visibility/activation overrides. Those Viewer overrides do not change upstream
render/write nodes, and their presence is visibly marked.

**Natron implication:** Stage Outline and a path picker are necessary, but
separate temporary inspection state from persistent graph edits. Do not make
an ordinary hide/isolate action silently alter final output. Point-ID selection
also needs element identities rather than pretending all selections are prims.

Utility rendering and Hydra are different contracts
----------------------------------------------------

The `rendering overview
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/render-scene.html>`_
lists ScanlineRender, GeoRender and SplatRender separately. The
`ScanlineRender workflow
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/rendering-scanline.html>`_
focuses on utility passes and shot integration, including reflections and
refractions, rather than replacing production rendering.

The `GeoRender workflow
<https://learn.foundry.com/nuke/17.1/content/comp_environment/usd-3d-comp/rendering-georender.html>`_
uses Hydra delegates, whose outputs/settings depend on the delegate. It
explicitly distinguishes USD materials from Nuke materials and notes asset
compatibility limits.

**Natron implication:** USD is the scene contract, not a promise that every
renderer understands every material/projection/AOV. Prioritize a fast utility
route for image placement and projection; retain hdNVP for its supported
lighting/interactions. Both consume the same scene. Measure whether hdNVP can
satisfy utility requirements before mandating it for all comp output. Do not
copy Nuke node names and infer backend equivalence from those names.

Decisions for the current proposal
----------------------------------

.. list-table:: Assessment against the proposed Natron design
   :header-rows: 1
   :widths: 29 16 55

   * - Area
     - Assessment
     - Required action
   * - Shared USD scene / SceneService
     - Retain
     - Keep evaluation independent of GUI and renderer; validate ownership
       and incremental updates locally. Nuke documents behavior, not this API.
   * - Native tracking / sparse landmarks
     - Retain
     - Make refine, masks, reprojection, persisted accepted results and
       plate/3D selection part of the first complete shot.
   * - CameraTracker directly outputs Scene
     - Revise
     - Keep image analysis output and create an explicit ReconstructionScene
       with a visible solve dependency and linked/baked result policy.
   * - Dense cloud always requires depth fusion
     - Revise
     - Start by validating dense correspondence/triangulation; keep depth
       conversion, fusion and meshing distinct.
   * - Point-assisted proxy / projection
     - Retain, specify
     - Add manual fit correction, stable element IDs, camera/lens-domain
       consistency and separate shader/sticky-UV operations.
   * - All comp rendering through one path tracer
     - Revise
     - Require a fast utility contract and capability-aware routing on the
       same scene; establish backend suitability with measurements.
   * - USD editing and branch merge
     - Strengthen
     - Explicit no-opinion authoring, binding strength/purpose, duplicate
       versus compose, path picker and temporary versus persistent visibility.

Validation still required
-------------------------

Documentation supports the workflow decomposition, not implementation quality.
Before committing to a solver/backend, run the reconstruction and composition
fixtures in :ref:`maint-3d-redesign`, then a Natron-only shot to final EXR.
Measure plate/geometry registration through the whole sequence, alpha/edge
quality, interactive latency, memory use, save/reload and linked/baked updates.

Nuke accuracy, stability, frame rate, production maturity of individual tools
and large-scene limits have not been measured here. Some newer nodes are
documented as beta/labs. External scene/material interoperability must be
tested per feature rather than inferred from the shared USD label.
