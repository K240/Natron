Qt 6 migration
==============

The supported build uses Qt 6 / PySide6 / Shiboken6 and CMake exclusively.
Qt 5 / PySide2 and qmake configuration files have been removed.

The migration replaces QRegExp with QRegularExpression, including wildcard
filters and Python syntax highlighting, and removes the Core5Compat
dependency. Wildcards use non-path conversion so node names and labels
containing slashes continue to match. Desktop DPI comes from QScreen.
Qt 6 mutex lockers, concurrent tasks, key combinations, tablet events,
view-item style options and native-event result types use the current APIs.

Python initialization uses PyConfig instead of deprecated global flags and
initialization setters. It preserves the configured interpreter's prefixes,
disables user site-packages and passes Natron's arguments without interpreting
them as Python options. Embedded scripts and project serialization keep their
existing APIs and formats.

See ``INSTALL_WINDOWS.md`` for the reproducible build. Linux/macOS packaging
and distribution installers still require platform-specific validation.
