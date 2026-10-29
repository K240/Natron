.. _maint-3d-redesign:

USD scene and camera reconstruction redesign
============================================

Status: design proposal, 2026-10-29. Code audit baseline: ``5344d7689``.
The product direction is USD-centered 3D compositing, including a camera
tracking, reconstruction and point-cloud workflow completed inside Natron.
The architecture and artist nodes below remain proposed. The first solver/USD
validation slice is implemented as described in :ref:`maint-camera-solve-validation`.
See :ref:`maint-3d-compositing` for implemented behavior.

The evidence and evaluation behind the revised workflow are recorded in
:ref:`maint-3d-workflow-review`, using Foundry's Nuke 17.1 documentation.

This proposal revises the evaluation architecture and implementation order in
:ref:`maint-3d-design`. Its projection, color, alpha and delivery requirements
continue to apply. Camera reconstruction becomes a primary acceptance workflow
rather than an extended interchange feature.

Artist workflow and first delivery
-----------------------------------

The first complete workflow is::

    Plate -> lens/image preparation -> CameraTracker -> image / 2D inspection
                                         : accepted solve dependency
                                 ReconstructionScene
                                         |
                              solved camera + sparse points
                                         |
                               AlignScene: ground/origin/scale
                                         |
                               select points -> CardFromPoints
                                         |
    Clean plate -> Project3D ----------> BindMaterial
                       ^                 |
                  SceneCamera         Render3D -> Merge with plate -> Write
                       ^                 ^
                       +-- aligned scene-+

Artists track and solve the shot, examine reprojection errors, select points
in either the plate or 3D view, establish the ground and scene scale, place
cards or proxy geometry, and project a clean plate from a chosen frame.
The shot camera, projection camera and free inspection camera have separate
roles. USD imports can be added to this same scene after alignment.

Foundry's `Camera Tracking workflow
<https://learn.foundry.com/nuke/content/comp_environment/cameratracker/camera_tracking.html>`_
connects tracking, solving, scene alignment and use of the solved scene.
Its `object placement guide
<https://modosdk.foundry.com/nuke/12.0/content/comp_environment/cameratracker/placing_objects_scene.html>`_
also uses selected solved points to position geometry. These motivate Natron's
acceptance workflow; they do not imply Nuke script or solver compatibility.

Distinguish two point-cloud products:

* **Sparse reconstruction:** triangulated landmarks associated with tracked
  image features and the solved cameras. Required for the first delivery.
* **Dense reconstruction:** additional image correspondences and triangulation
  using the plate and solved cameras. Follow with an explicit PointCloudGenerate
  operation, then optional surface reconstruction. Depth estimation/fusion is
  another possible backend rather than a required first dense implementation.

Foundry documents `dense point generation
<https://learn.foundry.com/nuke/current/content/comp_environment/point_clouds_meshes/connecting_pointcloudgenerator.html>`_
as a separate operation using footage and solved cameras. A sparse cloud is
useful for placement without being a surface, an occluder or a dense model.
Natron must not imply that solving the camera also reconstructs a mesh.

Implementation findings
-----------------------

The baseline already has branch-private composed stages, USD edit nodes,
Stage Outline, GPU prim/instance picking, MaterialX Project3D, progressive
shared-memory frames and utility image planes. Build on these foundations.

The architectural seams needing change are concrete:

* ``Engine/NativeScene3D.cpp::sceneAt(..., preserveUsd)`` evaluates different
  scene forms according to the renderer. USD edits and composed-camera access
  require hdNVP. ``Scene3DSnapshot`` combines cards, imported sources, edits,
  image providers and editor objects.
* ``Gpu/UsdSceneBridge.cpp`` authors the public stage as part of render export.
  Render3D, Scene3DViewer and StageOutline each own bridge state. Outline can
  compose without rendering, but repeats evaluation/export work.
* ``Gpu/UsdStageValue.cpp`` publishes immutable query values, but applies graph
  edits into one private root. There is no general node-by-node layer recipe
  or shared evaluation service. Instance-child edits automatically disable
  instancing for the selected instance.
* The hdNVP host retains its stage and delegate but transports scene changes
  through generated USD files and layer reloads. Camera separation and shared
  memory already exist; incremental layer/texture transport remains work.
* ``Engine/TrackerContext*`` provides 2D tracking and 2D transform fitting.
  ``libs/libmv`` contains reconstruction sources, but its CMake source list
  builds only a subset and omits the simple-pipeline reconstruction sources.
  Having these files and Ceres in-tree does not establish a working 3D solver.
* ``NodeDataType`` and ``NativeScene3D`` expose one typed output per effect.
  The design must provide scene-contained cameras/points and selectors rather
  than assume multiple physical output sockets already exist.

Scene evaluation independent of rendering
-----------------------------------------

Introduce a project-owned SceneService in the Engine-facing scene subsystem.
It owns evaluation, composition caches and queries for every 3D branch.
Render3D, Viewer and Stage Outline consume the service rather than assemble
their own public stages. Scene evaluation must work in NatronRenderer and
without a GPU or an hdNVP installation. Dependencies remain downward: Engine
and scene libraries do not include Gui.

Separate four responsibilities::

    Persisted graph + accepted tracking/reconstruction artifacts
                              |
                   immutable SceneRecipe DAG
                              |
                 SceneService / OpenUSD runtime
                              |
                      published SceneHandle
                    /          |           \
              Outline     Viewer tools    RenderSession
                                             |
                                  hdNVP or compatibility adapter

``SceneRecipe`` records source arcs, namespaces, node-authored opinions,
selection definitions, time mappings and image/artifact dependencies. It is
an immutable description of USD composition, not a second mesh scene graph.
The graph and persisted analysis artifacts remain authoritative. Generated
layers, composed stages and renderer caches are reproducible derived state.

``SceneHandle`` is opaque, branch-qualified and revisioned. Public APIs return
bounded plain-data query results or dependency descriptions, never mutable
UsdStage/UsdPrim handles. A request key includes project/branch identity, graph
and dependency revisions, frame/subframe, view and population/load policy.
An orbit-camera change changes a render request without changing scene identity.

Use an OpenUSD owner queue for mutations, load/unload, reload and composition.
Publish immutable snapshots; schedule queries so they cannot overlap writes
to layers shared by any dependent stage. This follows OpenUSD's
`threading model in the pinned 26.05 source
<https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/usd/usd/docs/multiThreading.md>`_.
UI queries carry the requested revision and cannot install stale results.

Native Card, Camera, Light and FBX author into the same USD recipe as ReadUSD.
Geometry, Camera and Material sockets remain useful typed scene fragments or
selectors. A point cloud is a scene primitive, not an image or triangle list.
Keep native units right-handed Y-up, local camera forward -Z. Preserve source
metadata and apply explicit import conversions, including camera dimensions.

Preserve the two import operations: shot-layer composition retains intentional
paths/opinion strength; asset assembly mounts a dependency-complete subtree
under a stable namespace. Arbitrary multi-root normalization still needs a
validated relationship/connection/asset-resolution plan. Reconstruction work
must not bypass these existing import restrictions.

Each editing node owns an immutable opinion-layer version with explicit
strength, target selection and provenance. Reuse shared upstream recipe DAG
nodes once. Distinct assembly operations create distinct identities; divergent
edits on a shared prim compose in defined order and remain inspectable.
Do not concatenate mutable stages or infer identity from vector positions.
Retain current Scene input-zero strength during compatibility migration.

Distinguish composing opinions from duplicating an asset. Merging two branches
that describe the same composed prim applies their opinions to that identity;
it does not imply two objects. An explicit assembly/reference operation creates
another namespace/instance. Show conflicts and strength in the inspector, and
test a split/modify/rejoin with visibility and material overrides.

Tracking and reconstruction data contracts
------------------------------------------

USD describes the scene result. Track observations, solver state and dense
analysis also need structured artifacts. Do not put all observations into USD
prim properties or encode them as camera animation curves.

.. list-table:: Analysis and scene values
   :header-rows: 1
   :widths: 23 77

   * - Value
     - Required content
   * - TrackSet
     - Stable track IDs; frame/view observations in full-resolution pixel
       coordinates; confidence, enabled/outlier state, masks, manual edits;
       source image fingerprint, format, PAR, crop and preprocessing identity.
   * - CameraModel
     - Intrinsics, principal point, image domain, lens model/version and
       parameters, distortion direction, film gate, shutter assumptions and
       sample-time mapping. Conversion to USD optics is explicit and tested.
   * - Reconstruction
     - Immutable solve ID and revision; TrackSet and CameraModel dependencies;
       camera extrinsics, landmarks linked to track IDs, per-observation/frame
       residuals, inliers, solved frame range, solver options/version/seed and
       scale/degeneracy status. Missing poses are explicit.
   * - Alignment
     - One versioned similarity mapping from reconstruction coordinates to
       scene space, ground/axis/origin constraints and optional known distance
       or survey constraints. Records whether physical scale is established.
   * - DenseCloud
     - Source solve and image dependencies, reference views, positions,
       stable element IDs, colors, normals when available, confidence,
       observation/support information and reconstruction settings.
   * - Scene fragment
     - USD cameras, UsdGeomPoints, generated geometry and relationships back
       to the appropriate immutable artifact and graph owner.

Define pixel origin, pixel-center convention and orientation at every boundary.
The existing libmv image adapter uses top-left image coordinates. Convert
full-resolution observations, proxy coordinates and Natron image windows in
one tested adapter. Solve residuals are reported in the declared pixel domain.
Use project FPS and source frame mapping explicitly; frame offsets, retiming,
PAR and crops cannot silently alter the camera calibration.

Store observations and accepted solves as versioned, content-addressed project
assets with relative paths and checksums; reserve ordinary cache eviction for
regenerable working data. Save As, project copying and packaging include these
assets. Undo references immutable revisions and retains referenced assets.
Reopening a project must recover the accepted result without rerunning a solve.

Tracking/solving are explicit cancellable analysis jobs. Capture dependencies
before launch; commit a complete result atomically to the requesting node only
if its dependency generation still matches. Source/settings changes mark the
accepted result stale while keeping its identity available for inspection.
Re-solving never runs implicitly during an ordinary image-render request.
Final jobs pin the selected accepted artifact and scene revisions.

Native camera solving
---------------------

CameraTracker presents Track, Solve, Refine and Scene controls in one tool.
Internally keep feature detection/tracking, camera estimation, bundle adjustment
and publication separate. Import existing Tracker markers as user observations
through an adapter; preserve the existing 2D Tracker's behavior and saved data.
Automatic feature replenishment, masking moving objects, exclusion of bad
tracks and manual correction are part of the camera-tracking workflow.

The first supported solve is a static rigid scene, one moving perspective
camera, constant intrinsics and global shutter. Start with an explicitly
undistorted/pinhole image domain. Provide known intrinsics and a separately
validated constant-focal estimation mode. CameraTracker must identify the
domain and offer the preparation needed for supported lens models inside
Natron; merely ignoring lens distortion is not a completed shot workflow.

Use a replaceable CameraSolveBackend interface. First validate the bundled
libmv reconstruction pipeline and current Ceres integration in a small native
target. Check missing source dependencies, detector availability, convergence,
callbacks/cancellation, coordinate conventions and maintenance cost. The
existing ``LIBMV_NO_FAST_DETECTOR`` build setting must be accounted for when
choosing feature detection. Reuse the existing region tracker where suitable;
do not presume AutoTrack supplies a finished automatic camera tracker.
Choose an alternative solver only from the same acceptance evidence, without
changing the published TrackSet/Reconstruction contracts.

Solve stages are feature/track validation, keyframe pair selection, robust
relative pose, triangulation, incremental camera registration, bundle
adjustment, outlier refinement and publication. Report registered/missing
frames, track lengths, residual distributions and per-frame reprojection
overlays. One global RMS value is insufficient to judge a shot.

Detect insufficient parallax, pure rotation, weak/planar configurations and
poor calibration. Do not publish arbitrary finite depths as a reliable
reconstruction. A later nodal/pure-rotation mode can publish orientation with
explicitly unavailable translation/depth. Zoom, rolling shutter, object motion,
stereo and multi-camera solving each require additional model/fixture work.

Monocular scale starts as arbitrary. Ground orientation and an origin do not
establish meters; a known distance or survey constraint is needed for physical
scale. Alignment transforms solved cameras, points and attached geometry
together. Preserve the raw solve and evaluate a separate alignment operation.
Normalize camera poses/optics through the shared camera adapter rather than
assuming that a scaled camera parent handles clip/optical units correctly.
Represent the uncalibrated scale state in the UI, exported metadata and any
dimensional outputs; USD stage units alone are not evidence of measured scale.

Point clouds and point-driven scene tools
------------------------------------------

Publish landmarks as UsdGeomPoints with explicit ``ids``, positions and display
color. Use standard primvars for suitable per-point quality fields and a
versioned Natron metadata contract for reconstruction/track relationships.
The `OpenUSD 26.05 geometry schema
<https://github.com/PixarAnimationStudios/OpenUSD/blob/v26.05/pxr/usd/usdGeom/schema.usda>`_
provides point positions, optional IDs and widths; it does not define a camera
solve or its observation database.

Keep one Points prim or bounded spatial chunks, not one prim/graph node per
landmark. Assign persistent point IDs through a stored track-to-int64 mapping.
Reordering, filtering and display LOD do not change identity. A changed solve
can remove landmarks; never retarget a saved selection using array indices.

Scene selections identify a branch/scene revision, prim path, instance context
and optional element kind/ID. Point tools use point IDs plus reconstruction
identity. UsdCollectionAPI can describe prim sets; point-ID sets need their own
selection artifact/attributes rather than pretending to be ordinary prim paths.
Prototype runtime paths and transient GPU pick IDs are not persistent IDs.

The Viewer needs a lightweight points/lines/wireframe tool pass that works
before path tracing and without hdNVP. It draws the plate backplate, solved
camera path/frusta, points, selections and reprojection residuals. Use bounded
point buffers, spatial LOD and revision-matched element picking. Drawing fewer
points must not restrict an explicitly requested full-resolution selection.
Stage Outline lists the cloud prim/chunks and summarized attributes; a
virtualized points inspector handles individual landmarks.

Default reconstruction clouds to guide purpose and exclude them from final
beauty, shadows and depth. Backplates are inspection/compositing inputs rather
than automatic environment lights. Opting into renderable points is a separate
operation with renderer capability validation; do not create a mesh or millions
of spheres just to make the cloud visible in the editor.

Required point actions are:

* Select in plate or 3D, with the same IDs and track correspondence. Filter by
  solve error, track support and enabled state; distinguish observed and
  reprojected points on frames without an observation.
* Set ground from at least three non-collinear points; set origin from a point,
  heading from a valid direction and scale from a known distance. Show fit
  residuals and reject degenerate constraints.
* Create Axis from a point or selection centroid. Fit a plane robustly to a
  valid point selection; expose normal direction, reference frame and extent.
  CardFromPoints uses that fit with an editable offset and size. A single point
  supplies position, with explicitly chosen camera/world orientation.
* Create simple proxy geometry and attach imported USD assets in the same
  aligned space. Allow manual position/orientation/extent correction against
  the plate, then later multi-frame vertex/object alignment. A complete general
  modeler or dense reconstruction is not required to place a useful proxy.
  Persist the selection, fit settings and graph operation for undo/save/reload.

Offer linked and baked attachments. A linked placement refers to stable track
IDs and the accepted solve and updates when that solve is replaced. Missing
support or an invalid fit leaves a visible unresolved attachment, without
silently choosing other points. Baked placement records the evaluated transform
and creation solve ID. Updating either mode is undoable; final render jobs pin
one consistent solve/alignment/attachment revision.

Dense reconstruction is a later part of the same architecture. PointCloudGenerate
uses the plate, mask, camera calibration and accepted camera poses. First
evaluate selected-keyframe dense correspondence and triangulation with baseline,
track-quality and spatial filtering, reusing the track/pose contracts. A later
backend can estimate/fuse multi-view depths with confidence/visibility checks.
Support bounded, cancellable processing and cached chunks. A PointsToMesh operation
requires explicit normals/support and surface-quality validation. Rendering
Depth/P from proxy geometry is useful for comp, but is not image-based dense
reconstruction of the photographed scene.

Node and UI organization
-------------------------

Keep CameraTracker an image-domain analysis node with an image output for plate
inspection, optionally using its declared lens preparation. An explicit Create
Scene action creates a ReconstructionScene result node referencing the accepted
solve. That node has one Scene output containing the solved camera and sparse
points. This respects Natron's single-output effect model without removing the
normal 2D plate workflow or changing an output's type according to a UI mode.

The result node's solve-source reference is a visible, serialized dependency
using the tracker node's stable UUID. It participates in dependency traversal,
hashing, invalidation, cycle checks and copy/paste UUID remapping. Linked results
follow accepted solve revisions; baked results pin persisted artifacts and can
survive removal of the analysis node. Create Scene is one undo transaction and
can generate the common alignment/camera-selection setup for the artist.

SceneCamera selects a camera prim from an upstream scene for existing camera
sockets, preserving its world transform and optics. Render3D can also select
a camera from its scene input. Provide live and frame/range snapshot modes;
avoid reinserting a duplicate camera when the intent is only to read one.
New public analysis socket types can follow if actual workflows require them.

Proposed operations are CameraTracker, ReconstructionScene, AlignScene,
SceneCamera, PointSelect, FitPlane, CardFromPoints, AxisFromPoints,
PointCloudGenerate and PointsToMesh.
Names are provisional. Selection/fit data can initially be owned by the scene
editing nodes instead of requiring an extra visible node for every internal
artifact. Keep the primary user sequence short.

Keep Card, Camera, Axis, Scene, TransformGeo, Project3D, BindMaterial and Render3D
as familiar scene operations. General transform/visibility/material controls
operate on selected scene prims regardless of native or imported origin.
Retain existing USDTransform/USDVisibility/USDCompositing IDs and old knob
semantics as compatibility adapters. Imported source files stay read-only.
Changing an instance descendant requires an explicit per-instance editable
operation with a visible loss of sharing, rather than silently changing its
instancing policy during a generic transform.

Imported-property editors distinguish no authored opinion, an authored value
and clearing this node's opinion. Showing an upstream value must not write it
back as an override. Expose material binding strength and purpose separately
from the target selection, preserving existing node behavior during migration.
Support a scene-path picker and common type selections before requiring artists
to type raw prim paths; dynamic masks and explicit point-ID sets have different
membership semantics.

Share selection among the plate overlay, 3D tools, Outline and track inspector.
Keep graph-node selection distinct from prim/point selection. Tool edits create
graph commands on the inspected branch, with one undo transaction per action.
Source pinning and camera inspection do not change the render camera. Tracking
and scene tools remain available while the Viewer displays a 2D composite.
Separate temporary Viewer hide/isolate state from graph-authored visibility,
activation and payload policies. Mark temporary overrides visibly and offer a
clear reset. A render-affecting edit creates/updates the appropriate graph node.

Rendering and process boundary
------------------------------

RenderSession consumes SceneHandle, camera selection, output format, requested
AOVs and quality intent. Query capabilities before starting a render. Include
points/curves, material networks, projection, ray roles, AOV semantics and lens/
shutter support; a USD file extension is not a renderer capability declaration.
Auto may choose another backend only if it preserves the requested semantics.
Keep explicit CPU/Vulkan compatibility rendering while parity is established.

USD scene ownership does not prescribe a single render engine. Provide a fast
comp-utility route for cards/projections, predictable alpha, depth and masks,
alongside the hdNVP/Hydra path for supported lighting/materials and interactions.
Both consume the same scene contract. Evaluate an hdNVP utility mode against
these requirements; if it fails them, implement a supported USD-to-utility
adapter rather than force all comp tasks through stochastic beauty rendering.
Do not introduce a second authoritative scene graph to obtain this distinction.

Preserve separate-process hdNVP ABI isolation. SceneService owns the public
composition; the worker owns a renderer-local mirror and all GPU state. Mirror
the same versioned recipe/layers, dependency identities, time and population
policy. Validate camera/path/binding/instance agreement in tests. These are
derived replicas of one scene contract, not independently authored scenes.

First route the existing file transport through SceneService. Then introduce
versioned layer replacement and property/texture updates with a base revision,
atomic apply/acknowledge, and full resynchronization on a mismatch. Do not add
an unrelated triangle-scene IPC format. Camera updates retain geometry and
textures; track overlays do not invalidate a beauty render. Changing the solve
or alignment invalidates dependent cameras, placements and projection results.

Retain shared-memory frame publication, cooperative cancellation and latest
preview scheduling. Track scene, camera, texture and output revisions separately
and bind picking to the displayed generation. Final Writer jobs pin immutable
dependencies and are unaffected by newer interactive preview requests.

Keep image-compositing and lit-utility intents on the same scene. Unlit plates
and projected paint must preserve working-space colors and premultiplied alpha;
data maps/AOVs receive no display transform or denoising. Share camera framing
across projection, reprojection, tools and final rendering, including PAR,
principal point, film gate, crop and overscan. Undo lens preparation on the
rendered insertion before the final distorted-plate merge when appropriate.

Current hdNVP color/depth/albedo/normal buffers and Natron's reconstructed P
remain compatibility outputs. Define sample/coverage/miss conventions before
promising exact surface-position, motion or reconstruction-derived depth passes.
Keep holdouts, catchers, motion blur, DOF and Cryptomatte as separately tested
delivery capabilities.

Implementation sequence and acceptance gates
--------------------------------------------

1. **Validate the solver and lock the camera/observation contracts.** Build a
   small reconstruction target with synthetic observations and a licensed shot.
   Confirm source/dependency closure and the existing tracker adapter. Exercise
   intrinsics, coordinate conversion, cancellation and weak/pure-rotation cases.
   Record solver error/runtime evidence before selecting the production backend.
2. **Extract SceneService and add point inspection.** Route existing native/USD
   evaluation and all three consumers through shared handles. Add UsdGeomPoints
   publication, stable point IDs, the guide/tool pass and element selection.
   A synthetic accepted solve must produce matching plate/3D selections and
   reprojection without starting hdNVP. Existing scene fixtures stay compatible.
3. **Deliver native CameraTracker to placement and projection.** Add feature
   tracking, solve/refinement UI, persistence, ReconstructionScene, AlignScene
   and point-driven Card/Axis creation. Test an actual Natron-only shot from plate to EXR, with
   an aligned USD asset, reference-frame projection and animated render camera.
   Synthetic import alone does not satisfy this gate.
4. **Harden interactive USD editing and render transport.** Generalize edit
   layers, make instance policy explicit, implement incremental updates and
   capability-based backend selection. Measure warm camera/texture updates,
   first preview, cancellation latency and retained CPU/GPU memory.
5. **Add dense point-cloud and surface tools.** Validate keyframe correspondence/
   triangulation, optional depth/fusion confidence, normals, chunk/LOD behavior,
   point-driven proxies and optional meshing.
   Add lens/solve modes, temporal passes and other render capabilities according
   to shot requirements and their own acceptance fixtures.

For reconstruction, evaluate noise-free synthetic reprojection, noisy/outlier
robustness, camera/point recovery up to similarity, held-out observations and
temporal stability. Define tolerances per fixture rather than claiming one RMS
threshold guarantees a good solve. Test non-square pixels, crops, frame offsets,
unregistered frames and stale jobs. Test ground/scale changes by checking that
camera and point reprojection agree before and after alignment.

For point tools, test reordered/filtered points, lost tracks after re-solving,
selection across display LOD, invalid plane fits, linked/baked attachments,
undo and project relocation with its analysis assets. Test large point sets
without a per-point prim/tree row/graph node or CPU BVH construction.

For USD migration, retain branch/source isolation, common-upstream provenance,
namespaced assets, cross-root bindings, payload/variant behavior, camera framing,
projection, alpha/data edges, save/reload and headless rendering. Existing node
IDs, scripts, renderer choices and saved image behavior remain stable through
versioned migration. Change Auto defaults only after demonstrated parity.

Immediate implementation boundary
----------------------------------

The first implemented slices below combine a small CameraSolveBackend validation
target with scene/query interfaces for synthetic and automatically tracked
camera-and-points results. They make solver feasibility, point identity and
renderer-independent inspection reviewable before broad render rewrites.
The overall completion criterion remains the native plate-to-solve-to-placement-
to-projection workflow, including persisted results and final image output.

.. _maint-camera-solve-validation:

First implemented solver and USD slice
--------------------------------------

The initial native validation is implemented in ``Scene/CameraSolve.h/.cpp``
and ``Scene/ReconstructionUsd.h/.cpp``. It uses the vendored libmv calibrated
two-view initializer, incremental reconstruction and Ceres bundle adjustment.
The reconstruction dependency closure is built as ``mv_reconstruction``;
the existing ``mv`` region-tracking target keeps its original sources.
The validated local configuration uses bundled Ceres 1.12.0 and OpenUSD 26.05.

``solveCamera`` accepts known pinhole intrinsics and enabled 2D observations
in a declared undistorted, full-resolution, top-left pixel domain. It preserves
fractional/offset frame numbers and 64-bit track IDs by remapping them to dense
internal indices. Results contain camera-to-world matrices, sparse landmarks,
reprojection residuals and explicitly unregistered frames. The first keyframe
defines the origin/orientation and the keyframe camera distance is normalized
to one; physical scale remains arbitrary.

``reconstructionUsdLayer`` authors an animated Camera and a guide-purpose
UsdGeomPoints cloud, including IDs, support/error/parallax primvars, image-domain
metadata and explicit arbitrary-scale status. It preserves unequal fx/fy and
an off-center principal point. Partial camera solves cannot be exported yet,
so a missing pose cannot silently become an interpolated camera in the asset.
The generated layer can be composed directly or saved as an asset and mounted
through the existing UsdSceneValue source/Outline/camera interface.

Run the CPU solver and USD integration checks after a normal Windows build::

    .\tools\run-windows-qt6.ps1 -Program CTest -ProgramArguments @('-R', '^CameraSolve', '-V')

Generate a portable synthetic USD scene with the same validated binary::

    .\tools\run-windows-qt6.ps1 -Program NatronCameraSolveUsd -ProgramArguments @('--output', 'build/camera-solve-validation/reconstruction.usda')

The authored synthetic fixture has eight camera samples, 64 non-planar points
and 512 observations. Tests compare recovered cameras/points with independent
ground truth up to similarity. A noisy run withholds 48 observations from the
solve and checks those projections separately. The local noisy training RMS
is approximately 0.112 pixels and held-out RMS 0.127 pixels; the USD camera's
own frustum reproduces noise-free observations within 0.00003 pixels.
These numbers describe this fixture, not real-footage solve accuracy.

Additional checks cover manual keyframes, disabled markers, duplicate/invalid
observations, pure rotation, a planar scene, missing frame poses, reprojection
rejection, cancellation between libmv operations, and USD assembly save/reload.
Existing USD composition/export and native Card/FBX tests remain acceptance
checks for this seam.

This first backend consumes inlier observations. The subsequent native workflow
below supplies automatic tracks, robust filtering, artist controls and shared
focal/radial lens estimation. The parallax/homography
checks conservatively reject weak selected keyframe pairs. Cancellation does
not interrupt an individual Ceres minimizer.

.. _maint-camera-tracker-native:

Native automatic tracking and CameraTracker
--------------------------------------------

``Scene/FeatureTracking`` now streams plate images through Harris detection and
pyramidal libmv patch tracking. Correlation and forward/backward consistency
reject failed matches; replenishment creates persistent 64-bit track IDs.
The robust solve searches keyframe candidates, estimates an epipolar model with
deterministic RANSAC, rejects mismatched keyframe tracks, and prunes high-error
observations between bundle adjustments. Every requested frame must register.
Homography degeneracy uses its own pixel threshold so a loose intermediate
bundle RMS cannot incorrectly classify all correspondences as planar.

The built-in ``CameraTracker`` node exposes the native workflow:

#. Connect an undistorted plate to Source. Optionally connect an inclusion mask
   (alpha for RGBA images, the single channel otherwise; values below 0.5 exclude).
#. Set First/Last frame and tracking controls, then click **Track features**.
#. Supply known focal length/sensor width, or enable calibrated pixel intrinsics
   and enter fx/fy/cx/cy in full-resolution top-left pixel-edge coordinates.
   OpenCV integer pixel-center coordinates require a +0.5 principal-point shift.
#. Click **Solve camera**. Automatic or explicit keyframes are supported.
   The status reports registered frames, sparse-point count and RMS.
   **Show track IDs** identifies observations for the space-separated Exclude
   track IDs control. Green markers are tracks; orange markers are reprojections.
#. Click **Create USD scene** to create linked ``ReconstructionScene``,
   ``SceneSetup``, ``SceneCamera``, ``Render3D`` and, in the GUI, Viewer nodes.
   The original or lens-corrected plate is connected as the Render3D background.
   SceneSetup aligns the solve and places geometry in its shared coordinate
   frame, as described in :ref:`maint-reconstruction-layout`.

Track/Solve run on worker threads with main-thread progress updates. Cancelled
or superseded jobs cannot replace the accepted analysis. Source/mask parameter
ages, upstream connections and settings are captured; changing them marks the
result stale and prevents solving/creating a scene from mismatched data.
The source signature excludes project creation time so reload retains validity.
Results use ordinary versioned, compressed node parameters and support Undo/Redo.
The last accepted solve remains available for inspection until replaced.

``ReconstructionScene`` has an explicit CameraTracker input dependency. It
regenerates a content-addressed USD cache from the saved analysis, mounts the
camera/point cloud under its stable source identity, and contributes guide points
to native 3D inspection. Guide points draw in the 3D Viewer and participate in
framing and Outline bounds; they do not render as scene geometry.
``SceneCamera`` queries the composed USD stage and retains both film apertures,
including unequal fx/fy and off-center calibration, through CPU/Vulkan projection
and USD authoring. The solved camera is read-only in the 3D gizmo; use scene
transforms for placement. Existing cameras retain their horizontal-fit behavior.

This is a static-scene, single-view workflow. The auto-calibration extension
below estimates focal length and radial distortion. SceneSetup provides ground,
origin and known-distance alignment, point selection and geometry placement.
It does not model rolling shutter.
Pure rotation, planar scenes and insufficient parallax can be rejected. Dense
reconstruction is outside this slice. Reprojection RMS measures fit to the
accepted tracks and is not independent physical camera accuracy.

Reproducible real-plate validation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The optional real test uses the RGB preview of **Freiburg3 long office household**
from the `TUM RGB-D Dataset <https://cvg.cit.tum.de/data/datasets/rgbd-dataset>`_,
credited to Jürgen Sturm and the TUM benchmark contributors, under CC BY 4.0.
The `published calibration <https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats>`_
identifies Freiburg3 images as already undistorted. Neither depth nor ground-truth
poses are inputs. The download tool verifies the movie checksum and writes
source/license/derivation metadata beside the decoded PNGs. Media stay in the
ignored build directory, outside the repository.

Prepare the data and run the headless and owned-profile GUI workflows::

    uv run --frozen --with imageio-ffmpeg python tools/prepare-camera-tracker-test-data.py
    .\tools\run-windows-owned-gui-test.ps1 -Program NatronRenderer -Script tools/camera-tracker-smoke.py -Profile build/test-profiles/camera-tracker-native -Result build/camera-tracker-real/native-result.json
    .\tools\run-windows-owned-gui-test.ps1 -Script tools/camera-tracker-gui-smoke.py -Profile build/test-profiles/camera-tracker-gui -Result build/camera-tracker-real/gui-result.json

Use fresh owned-profile directories on each run. Both workflows default to all
45 prepared frames; ``NATRON_CAMERA_FRAMES`` can select a shorter range. The
known-camera run solves 45 cameras / 164 landmarks at RMS 0.250 pixels.
Save/reload retained the accepted USD and validity;
the headless test rendered a card placed at a solved landmark through Render3D.
GUI checks cover actual buttons, cancellation, responsive event processing,
Track/Solve Undo/Redo, stale-result rejection, the read-only camera gizmo, guide
point display and the composed Camera/Landmarks Outline hierarchy.

``FeatureTrackingCpu`` checks independent synthetic subpixel flow (about 0.037
pixel mean error), mask exclusion, cancellation and a noisy solve with deliberately
mismatched tracks. ``CameraSolveUsd`` also checks the calibrated CPU renderer's
rays against independently generated observations, including unequal fx/fy.

Unknown cameras and radial lens estimation
--------------------------------------------

CameraTracker's **Camera calibration** choice now offers **Known camera**,
**Estimate focal** and **Estimate focal + radial**. Known camera remains the
default and preserves existing projects. For an unknown plate, select an estimate
mode, track the source as usual and click Solve camera; focal/sensor metadata is
not required. Auto modes start with a centered principal point, the source pixel
aspect and one constant lens throughout the shot. Optional principal-point
refinement is described below. They estimate focal length in
pixels; a physical focal length in millimeters cannot be recovered without a
sensor size. The USD camera uses a 36 mm equivalent gate to preserve its field
of view, not to claim measured physical lens dimensions.

``Scene/CameraIntrinsics.cpp`` initializes from six focal hypotheses through
the existing robust libmv reconstruction. Ceres then jointly refines a shared
log focal length, camera poses and landmarks, followed optionally by Brown
radial k1/k2. The fy/fx ratio remains fixed; the principal point is fixed by
default or refined through the validation step below. A fixed anchor pose and
one translation component remove the similarity gauge during optimization;
the accepted result restores the unit keyframe baseline. Huber residuals,
bounded focal/lens parameters, inlier support and positive-depth/parallax checks
reject unusable estimates. Focal search is bounded to 0.25..4 image widths.
The radial model is selected only when the clipped residual score improves by
more than 0.02 pixels relative to the simpler focal-only model. Iteration callbacks
allow cancellation within the new Ceres refinements.

For normalized ideal coordinates u/v and radius squared r2=u*u+v*v, the model
is ``distorted = ideal * (1 + k1*r2 + k2*r2*r2)`` before applying fx/fy and the
principal point. Known camera also accepts supplied k1/k2. The inversion rejects
noninvertible observations; a monotonicity check rejects folded image domains.
Reprojection errors and 2D overlays are evaluated in the original distorted plate
domain, rather than comparing an ideal projection directly with distorted tracks.

USD's Camera remains an ideal pinhole camera. Create USD scene connects an
**UndistortPlate** node between the source and Render3D background for estimated
lenses or nonzero supplied distortion. Its second input explicitly references
CameraTracker's accepted analysis; it applies a bilinear pull warp in linear
image data, preserving source format, origin, pixel aspect and alpha, including
proxy coordinate mapping. Missing source coverage is black; this first version
does not expand the format for overscan. A zero-distortion estimate produces an
identity correction. The distortion coefficients and domain/observability flags
are saved in the USD root's Natron metadata. Other USD consumers must explicitly
apply the recorded plate correction; ordinary USD camera optics do not implement it.

Analysis schema version 2 persists lens coefficients, image dimensions and
estimation/observability diagnostics. Version 1 projects load as zero-distortion
known cameras. Undo/Redo restores the complete accepted lens, camera and USD
state together. Estimated values are displayed separately in Analysis status;
manual calibration fields are disabled in automatic modes.

Low reprojection RMS alone does not establish a unique lens. Competitive starts
with a focal spread above 20 percent, differing corner corrections, or less than
eight degrees of recovered camera rotation mark the result **weakly constrained**.
This is a conservative diagnostic, not a statistical confidence interval. It is
displayed in the UI and persisted in USD. Add rotation, depth and image-radius
coverage, or supply known calibration, before relying on an ambiguous lens.
Centered-principal-point and fixed-aspect assumptions can also bias estimates
for cropped/decentered footage. Zoom, tangential distortion, fisheye models and
rolling shutter remain outside this implementation.

``CameraIntrinsicsCpu`` tests an independently generated 12-camera scene with
known focal and radial distortion, withholds observations from fitting, checks
focal-only estimation with non-square pixels, and checks cancellation, inversion
and a translation-only critical motion. The latter must be marked ambiguous.
``CameraSolveUsd`` checks that distortion metadata leaves USD's ideal camera
projection unchanged and records the required plate correction.

Run optional real-plate tests without supplying any published intrinsics::

    $env:NATRON_CAMERA_AUTO = '1'
    .\tools\run-windows-owned-gui-test.ps1 -Program NatronRenderer -Script tools/camera-tracker-smoke.py -Profile build/test-profiles/camera-auto-native -Result build/camera-tracker-real/auto-result.json -TimeoutSeconds 180
    .\tools\run-windows-owned-gui-test.ps1 -Script tools/camera-tracker-gui-smoke.py -Profile build/test-profiles/camera-auto-gui -Result build/camera-tracker-real/auto-gui-result.json -TimeoutSeconds 180
    Remove-Item Env:NATRON_CAMERA_AUTO

These use the first 45 prepared RGB frames by default; set
``NATRON_CAMERA_FRAMES`` to choose a shorter prepared range. They test estimated
calibration, the linked correction, project reload and image output, as well as
the interactive controls and overlays. Known-camera tests use the same 45-frame
default so regenerated samples cover the entire prepared plate. Real-plate fit
results are not independent camera accuracy measurements; the synthetic held-out
test supplies that separate check.

The local 45-frame unknown-camera run retained 168 tracks / 7309 observations
and solved 45 cameras / 166 landmarks at RMS 0.282 pixels. It selected a
zero-distortion model with focal 562.77 pixels and correctly marked the lens
weakly constrained. This differs from the published calibration and must not
be treated as independently recovered physical lens truth. The separate synthetic
run recovered focal 809.98 for a truth of 810, k1=-0.11995 and k2=0.03493 for
truths -0.12 / 0.035, with 0.0244 pixel RMS on 111 withheld observations.
The prescribed image-warp check compares independent Brown pixel coordinates
against rendered PNG samples on smooth regions, including an off-center
principal point and unequal fx/fy. Its tolerance is four 8-bit code values to
allow linear-light filtering and quantization; the local maximum error is 1.09.

.. _maint-camera-principal-point:

Optional principal-point refinement
--------------------------------------

Enable **Refine principal point** with either automatic calibration mode to
compare the centered model with a freely estimated cx/cy. This addresses cropped
or decentered plates when the observations support the extra parameters. The
option is off by default, disabled in Known camera mode, and preserves earlier
automatic solves when off. The principal point is bounded to 35..65 percent of
each image dimension; larger offsets can still be supplied explicitly in Known
camera mode.

After multi-start focal/radial initialization, observations are sorted by frame
and stable track ID. One active observation in eleven is withheld from the new
refinement objectives. The centered reference and alternative principal-point
models fit the same remaining observations. Alternatives include focal-only and,
when enabled, radial distortion. An alternative must improve clipped validation
RMS by more than 0.01 pixels, with at least 30 validation observations. The
selected model is refit using the full observation set and rechecked for camera
coverage, reprojection error, parallax and lens validity.

This model-selection diagnostic uses the existing solve for initialization; it
is separate from the independent withheld-data accuracy test. Enabling the
option does not force a shifted principal point. Analysis status reports whether
refinement was adopted and its validation gain, or why the centered model was
kept. Motion/candidate ambiguity diagnostics remain active, including a warning
when an estimated principal point approaches its bounds.

The existing version 2 analysis stores the additional optional
``estimatedPrincipalPoint`` and ``principalPointValidationGain`` diagnostics.
The calibration array already includes cx/cy, so its format is unchanged.
Previous version 1/2 analyses default to the original fixed-principal-point
behavior. Undo/Redo, project reload, the USD camera's aperture offsets and
UndistortPlate all use the accepted cx/cy. USD metadata records the estimation
flag and validation gain.

The synthetic test shifts the true principal point from (480, 360) to (460, 380)
in a 960x720 plate. The local run recovers approximately (459.94, 380.11) and
focal 809.97 pixels for a truth of 810, with 0.0244 pixel RMS on observations
excluded from all reconstruction/refinement fitting. Additional checks reject
an unnecessary principal-point model on a centered camera, preserve the result
when observations are reordered or disabled, and cancel the principal-point
phase. The image correction test also exercises noncentered optics.

Set ``NATRON_CAMERA_PRINCIPAL=1`` along with ``NATRON_CAMERA_AUTO=1`` for the
optional native/GUI smoke tests above. On the existing 45-frame TUM sample,
validation did not justify changing the principal point: the implementation
kept (320, 240), retained the weak-lens warning and preserved the previous
45-camera / 166-point result at RMS 0.282 pixels. This clip does not establish
that freely changing the principal point improves physical camera accuracy.

.. _maint-reconstruction-layout:

Reconstruction layout and projected ground
------------------------------------------

The first 3D layout milestone connects a solved plate to editable geometry
inside Natron. **Create USD scene** now inserts ``SceneSetup`` between
``ReconstructionScene`` and both ``SceneCamera`` and Render3D's scene input.
Existing projects that connect ReconstructionScene directly remain valid.

#. Enable **3D**, then **Points** in the Viewer (``P`` also switches Points
   mode). Click a landmark to select it; Shift-click adds or removes points.
   The selected points show their persistent track IDs and reveal SceneSetup's
   controls. **Camera** shows the points through the solved camera; ``F`` in
   Points mode frames the selected landmarks in the inspection view.
#. Select at least three non-collinear points on the same ground/table plane
   and click **Set ground from selection**. The fitted plane becomes Y=0;
   the first two points define the X direction. Coincident, collinear and
   substantially non-planar selections are rejected without changing the frame.
#. Select one or more landmarks and click **Set origin from selection**.
   The centroid becomes the origin; after ground alignment only X/Z translate,
   preserving ground at Y=0. Select exactly two landmarks, enter their measured
   **Known distance (m)** and click **Set scale from two landmarks** to establish
   metric scale. A monocular solve alone does not provide that measurement.
#. Select an anchor landmark or group of landmarks. **Create axis at selection**
   and **Create card at selection** create ordinary editable native nodes.
   Set **USD asset** and **Asset scale**, then **Place USD asset on ground** to
   create a ReadUSD in Asset reference mode. The asset bounds are centered on
   the selected centroid in X/Z; their lowest Y rests on the aligned ground.
   With no ground alignment, the bounds rest at the selected centroid's Y.
   Coordinates and bounds use the existing Y-up, meter-normalized USD reader.
#. Set **Projection reference frame** and **Card / ground size**, then click
   **Create projected ground**. This creates a ground Card, frozen Project3D
   and SceneCamera. Project3D uses the target Render3D's background, including
   UndistortPlate when the accepted solve requires it. The projection camera
   reads the upstream scene before SceneSetup. It therefore shares alignment
   without forming a graph feedback loop. The image and camera freeze at the
   reference frame, using the existing Project3D timing controls.

New geometry is merged **upstream** of SceneSetup. Its placement controls are
converted back to that input coordinate system; SceneSetup applies one common
positive uniform similarity to cameras, guide points, geometry, projectors and
lights. Changing ground/origin/scale after placement preserves camera projection.
Native transform controls and Viewer gizmos remain editable; the gizmo accounts
for the shared parent frame, with asset dragging and its Undo/Redo covered by
the GUI regression. ReadUSD retains an external reference, leaving
the source asset file unchanged. Add Scene lights or an environment light for
USD materials that require illumination.

The reconstructed USD camera is republished with transformed positions and a
rigid rotation basis. Uniform world scale changes its position/clipping ranges,
not its film apertures, focal value or rotation basis. Guide-point IDs and
quality metadata are retained; metric/arbitrary status is recorded in USD.
The versioned scene-frame parameter and selected IDs persist in the project.
The cache is regenerated from saved analysis and does not become a project
dependency. Ground/origin/scale changes undo as one parameter operation; each
placement restores all its nodes and the SceneSetup connection as one operation.
Status follows frame Undo/Redo and project reload.

SceneSetup accepts exactly one reconstruction branch. It records the accepted
solve's content identity when aligning or placing geometry. Replacing that solve
invalidates the old layout and requires **Reset scene frame** and a review of
placed geometry before further setup. Reset clears alignment while retaining
the ordinary scene nodes. A manually created SceneSetup needs its hidden
``layoutRenderer`` target identity and the corresponding Render3D scene connection;
the Create USD scene action supplies these automatically.

Validation uses the RGB-only TUM sample above, with tabletop landmarks 33, 49
and 75. A demonstration constraint of 0.8 m establishes a chosen layout scale;
it is not a published ground-truth measurement. The local run produced the same
image before/after alignment and after project reload. Rescaling the completed
layout changed 53 of 1,228,800 image channels, by at most 11/255, at precision-
sensitive renderer samples; camera/point reprojection is independently checked
below 1e-8 pixels by ``SceneFrameCpu``. The UI test exercises actual point clicks,
Shift toggling, alignment controls, placement, multi-node Undo/Redo, asset gizmos,
keyboard mode synchronization and composed USD geometry/materials in the Outline.
Run after ``camera-tracker-smoke.py``, with fresh profiles::

    .\tools\run-windows-owned-gui-test.ps1 -Program NatronRenderer -Script tools/reconstruction-layout-smoke.py -Profile build/test-profiles/layout-native -Result build/camera-tracker-real/layout-native-result.json
    .\tools\run-windows-owned-gui-test.ps1 -Script tools/reconstruction-layout-gui-smoke.py -Profile build/test-profiles/layout-gui -Result build/camera-tracker-real/layout-gui-result.json

The sample project and rendered composite are written under
``build/camera-tracker-real/reconstruction-layout.ntp`` and
``build/camera-tracker-real/layout-composite.png``. The GUI run saves a project
named after its owned profile, with a connected Viewer and reviewable controls.
Validation also exposed an existing desktop shutdown ordering bug: NodeGui
teardown accessed shared clipboard state after it had been destroyed. Desktop
shutdown now closes instances while the GUI manager's state is still alive;
``tools/gui-shutdown-smoke.py`` checks clipboard use and a clean process exit.
The full existing 3D Viewer GUI regression also exits successfully.

Projection supplies a plane
for set extension; it does not reconstruct the plate's occluding geometry.
The projected plate material is unlit. Native depth ordering applies, and lit
materials can use the existing lighting and shadow rendering. Dedicated shadow
catchers and plate proxies are now provided by :ref:`maint-3d-shadow-catcher`;
reflection catchers now have a dedicated workflow in
:ref:`maint-3d-reflection-catcher`. Individual composed
USD Prims can now be manipulated through USDTransform's Viewer gizmos and
persisted pivot; see :ref:`maint-3d-stage-outline`. This milestone does not add
dense reconstruction or a Nuke script importer.
