---
name: natron-maintainer
description: Domain knowledge for working on the Natron compositor's own C++ source code (Engine/Gui/HostSupport). Use when navigating, fixing, or extending the Natron codebase, doing Qt5/Qt6 work, building Natron, building its Sphinx docs, or triaging its GitHub issues.
version: 1.0.0
tags: [skill, natron, compositor, openfx, qt, cpp]
---

# Natron Maintainer

## Overview
Natron is a node-based OpenFX-host video compositor. Its code is a stack:
OpenFX host (``HostSupport`` + ``libs/OpenFX``) â†’ engine (``Engine``) â†’ Qt GUI
(``Gui``), with thin ``App``/``Renderer`` entry points. Dependencies point
**downward only**: ``Engine`` must never include a ``Gui`` header (so the
headless ``NatronRenderer`` can link ``Engine`` without ``Gui``).

## Usage
Use this skill when:
- Navigating or modifying the Natron C++ source (Engine/Gui/Global/HostSupport).
- Adding or fixing a node, knob (parameter), render behavior, or the cache.
- Doing Qt 5 / Qt 6 migration work or touching the Python (Shiboken/PySide) bindings.
- Building Natron, running its tests, or building the Sphinx documentation.
- Triaging or prioritizing Natron GitHub issues.

The authoritative long-form reference is the in-repo **Maintainer Guide** at
``Documentation/source/maintainers/*.rst``. Read the relevant chapter there for
depth; this skill is the quick map.

## Core Concepts

- **Runtime object tree:** ``appPTR`` (``AppManager`` singleton) â†’ ``AppInstance``
  (one per project) â†’ ``Project`` (a ``NodeCollection`` + ``KnobHolder``) â†’
  ``Node`` â†’ ``EffectInstance``. A ``Node`` is the stable graph vertex; its
  ``EffectInstance`` is the (replaceable) behavior â€” either an
  ``OfxEffectInstance`` (an OpenFX plug-in) or a built-in C++ effect
  (``ViewerInstance``, ``RotoPaint``, ``TrackerNode``, ``ReadNode``/``WriteNode``,
  ``NodeGroup``, ``Backdrop``, ``Dot``, â€¦).
- **Rendering is pull-based:** an output effect calls
  ``EffectInstance::renderRoI(RenderRoIArgs&, â€¦)`` which recurses upstream by
  region of interest, time, view and mip-map level; results are stored in a
  hashed RAM/disk ``Cache``. Per-render context lives in thread-local
  ``ParallelRenderArgs`` installed by ``ParallelRenderArgsSetter``.
- **Params are "knobs":** ``KnobI`` â†’ ``KnobHelper`` â†’ concrete ``KnobInt`` /
  ``KnobDouble`` / â€¦ ; owned by a ``KnobHolder``; created via
  ``appPTR->getKnobFactory().createKnob<K>(...)``. Knobs are NOT ``QObject`` â€”
  each has a ``KnobSignalSlotHandler`` companion for signals.
- **Natron IS an OpenFX host:** most nodes are OFX plug-ins; ``Engine/Ofx*`` glues
  the generic host in ``HostSupport``/``libs/OpenFX`` to Natron's node/knob/image
  model.

- **The two executables** share the Engine (``Gui`` links it; ``NatronRenderer``
  links it without ``Gui``). A third binary, ``natron-python``, is a full Python
  interpreter with Natron's modules; it also installs PyPI packages into Natron's
  bundled Python via ``natron-python -m pip install <package>``.

## File-Naming Conventions (learn these first)

| Suffix | Meaning |
|--------|---------|
| ``FooFwd.h`` | Forward decls + ``FooPtr``/``FooWPtr`` typedefs. ``Engine/EngineFwd.h`` and ``Gui/GuiFwd.h`` are the master catalogs â€” start here for unknown types. |
| ``FooI.h`` | Abstract interface (pure virtual). The Engineâ†”Gui seam: ``NodeGuiI``, ``KnobGuiI``, ``OpenGLViewerI``, ``NodeGraphI``, ``DockablePanelI``. Engine holds these; Gui implements them. |
| ``FooPrivate.h/.cpp`` | PIMPL implementation of ``Foo`` (public header holds only ``_imp``). |
| ``FooSerialization.h`` | Boost.Serialization (XML) description; version with ``BOOST_CLASS_VERSION``. |
| ``PyFoo.h/.cpp`` | Python-facing facade (Shiboken-bound). |
| ``OfxFoo.h/.cpp`` | OpenFX host glue. |
| ``Gui20.cpp``, ``ViewerTab30.cpp`` â€¦ | One big class split across numbered files; numbers are grouping only. |

## Key Idioms

- Everything is inside ``NATRON_NAMESPACE_ENTER``/``_EXIT`` (macros in
  ``Global/Macros.h``); Python-exposed classes use ``NATRON_PYTHON_NAMESPACE``.
- **Include ``<Python.h>`` first** in every TU (the "PYTHON BLOCK"); it must
  precede standard headers.
- ``std::shared_ptr``/``weak_ptr`` everywhere; parentâ†’child is ``shared_ptr``,
  childâ†’parent is ``weak_ptr`` (avoid cycles). Add new types to the ``Fwd``
  catalog.
- ``QT_NO_CAST_FROM_ASCII`` is set: wrap literals in ``QString::fromUtf8("â€¦")``
  or ``tr()``.
- Serialization and the ``Py*`` API are **backward-compatibility-critical** â€”
  version serialization changes; keep the Python API stable.

## Quick Reference

Build (CMake, Qt 6 only):
```powershell
.\tools\build-windows-qt6.ps1
.\tools\run-windows-qt6.ps1 -Program CTest
```
See `BUILDING.md` for other platforms. Python tools use uv; match the Qt SDK,
PySide6 and Shiboken6 versions. Qt 5 and qmake have been retired.

Code style (enforced by ``.git-hooks/pre-commit``):
```bash
astyle -p -H -f -j -z2 -c -k3 -U -A8 -n path/to/File.cpp && git add path/to/File.cpp
```

Docs (Sphinx; sources in ``Documentation/source``):
```bash
cd Documentation && sphinx-build -b html source html
```
Do NOT hand-edit ``index.rst`` (except to add a whole new guide to the toctree),
``_``-prefixed files, or ``plugins/`` (generated by ``tools/genStaticDocs.sh``).

## Qt 6 Migration Status

- CMake requires Qt 6.6+, PySide6 and Shiboken6.
- QRegularExpression replaces QRegExp; Core5Compat is not required.
- QScreen, Qt 6 mutex lockers, concurrent lambdas and event APIs are used.
- Python embedding uses PyConfig; build and dependency tooling use uv.
- The Windows MSVC workflow is validated locally; other platforms and installer
  packaging require separate validation.
- Details: `Documentation/source/maintainers/qt6-migration.rst`.

## Issue Triage Method

Fetch open issues via the GitHub API and aggregate by label:
```bash
curl -s "https://api.github.com/repos/NatronGitHub/Natron/issues?state=open&per_page=100&page=1"
```
Exclude items with a ``pull_request`` key (those are PRs). Labels: ``type:*``,
``func:*``, ``prio:*``, ``difficulty:*``, ``status:*``. Prioritize:
**P0** stability/data-loss (crashes, cannot-launch, cannot-render), **P1**
sustainability (Qt6, CI, distribution), **P2** confirmed functional bugs +
popular features, **P3** polish. Full analysis:
``Documentation/source/maintainers/issue-triage.rst``.

## Common Mistakes
- Including a ``Gui`` header from ``Engine`` (breaks the headless renderer). Use a
  ``â€¦I`` interface instead.
- Reading live node state during a render instead of the captured hash/args in
  ``ParallelRenderArgs`` (causes inconsistent renders / stalls).
- Changing a ``*Serialization`` struct without a version bump (breaks users'
  project files).
- Making a core value-type a ``QObject`` for signals â€” use the
  ``KnobSignalSlotHandler`` companion pattern.
- Reintroducing Qt 5 / qmake paths; CMake is the supported build system.
