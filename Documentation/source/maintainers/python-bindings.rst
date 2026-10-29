.. _maint-python:

Python Bindings (Shiboken / PySide)
===================================

Natron embeds a Python 3 interpreter and exposes much of its API to Python. This
powers parameter expressions, user scripts and callbacks, PyPlugs, and custom
GUI panels — everything documented in the :ref:`developers-guide`. This chapter
covers the *machinery* that produces those bindings, which maintainers must
regenerate when the C++ API changes.

The pieces
----------

- **The ``Py*`` C++ classes** (``Engine/PyNode.*``, ``PyParameter.*``,
  ``PyRoto.*``, ``PyTracker.*``, ``PyAppInstance.*``, ``PyNodeGroup.*``,
  ``PyExprUtils.*``, ``PyGlobalFunctions.*``; ``Gui/PyGuiApp.*``,
  ``PythonPanels.*``). These are deliberately small, stable, scripting-friendly
  facades over the engine/GUI objects. They live in the
  ``NATRON_PYTHON_NAMESPACE`` (see :ref:`maint-design`). The Python API surface
  is defined by these classes — not by the whole engine.
- **Shiboken** — the binding generator (part of PySide). It reads the C++
  headers plus a *typesystem* XML file and emits C++ "wrapper" sources that
  expose the classes to Python.
- **The typesystem files** — ``Engine/typesystem_engine.xml`` and
  ``Gui/typesystem_natronGui.xml`` (plus ``Shiboken/typesystem_widgets.xml``).
  They tell Shiboken which classes/functions to expose, how to map types, what
  to rename, and what to skip. When you add or change a ``Py*`` class, you edit
  the matching typesystem file.
- **The generated wrappers** — ``natronengine_*`` and ``natrongui_*`` wrapper
  sources produced into the build directory by the Engine and Gui CMake
  targets. They are build artifacts, not hand-edited.
- **The PySide module glue** — ``Pyside_*_Python.h`` and the version-specific
  ``PySide6_*_Python.h`` headers provide the matching Qt 6 bindings.

Qt 6 binding toolchain
----------------------

The CMake build requires **Shiboken6 / PySide6**; Qt 5 / PySide2 and the
``NATRON_QT6`` selection switch have been retired (see :ref:`maint-building`).
Match the Qt SDK, PySide6 and Shiboken6 versions and regenerate the bindings
when changing that toolchain. A mismatch can cause the
"``Qt.Alignment`` flags unrecognized" class of errors (issue
`#854 <https://github.com/NatronGitHub/Natron/issues/854>`_). See
:ref:`maint-qt6`.

Working on the bindings
-----------------------

- To expose a new method or class to Python: add/adjust the ``Py*`` facade in
  C++, then declare it in the relevant ``typesystem_*.xml``, then rebuild so
  Shiboken regenerates the wrappers.
- Keep the ``Py*`` API **stable and minimal**. User scripts, PyPlugs and the
  official tutorials depend on it; breaking it breaks users' projects just as
  surely as breaking the serialization format.
- The reference documentation for the Python API under
  ``devel/PythonReference`` is generated from these bindings; regenerate it when
  the API changes (see the documentation build notes in :ref:`maint-contributing`).
- To add a third-party Python dependency for scripts, callbacks or PyPlugs,
  install it into Natron's bundled interpreter with
  ``natron-python -m pip install <package>`` rather than the system Python — this
  keeps the package on the same Python that Natron actually runs.
