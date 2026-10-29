# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <https://natrongithub.github.io/>,
# (C) 2018-2023 The Natron developers
# (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
#
# Natron is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Natron is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****

include(FindPkgConfig)

pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo fontconfig)

file(GLOB NatronEngine_HEADERS *.h)
file(GLOB NatronEngine_SOURCES *.cpp)

set(PYENGINE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Qt${QT_VERSION_MAJOR})
set(PYENGINE_INCLUDE_DIRS ${PYSIDE_INCLUDE_DIRS} ${PYSIDE_INCLUDE_DIRS}/QtCore)
list(APPEND PYENGINE_INCLUDE_DIRS ${QTCORE_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
list(TRANSFORM PYENGINE_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE PYENGINE_INCS)
set(PYENGINE_HEADER PySide${PYSIDE_VERSION_MAJOR}_Engine_Python.h)

set(shiboken_args
    "-std=c++17" "--enable-parent-ctor-heuristic" "--use-isnull-as-nb_nonzero"
    "--avoid-protected-hack" "--enable-pyside-extensions"
    "-I." "-I.." "-I../Global" "-I../libs/OpenFX/include" ${PYENGINE_INCS}
    "-T${PYSIDE_TYPESYSTEMS}" "--output-directory=${PYENGINE_OUT}"
    "${PYENGINE_HEADER}" "typesystem_engine.xml")
if(WIN32)
    list(PREPEND shiboken_args "--clang-options=-DNOMINMAX,-DNATRON_BUILD_NUMBER=0")
endif()

execute_process(COMMAND ${NATRON_PYTHON_COMMAND} ../tools/utils/sourceList.py typesystem_engine.xml ${PYENGINE_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PyEngine_SOURCES)
set_property(SOURCE ${PyEngine_SOURCES} PROPERTY SKIP_AUTOGEN ON)
# Natron's injected binding code uses the full CPython API.
set_property(SOURCE ${PyEngine_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
    "$<IF:$<CXX_COMPILER_ID:MSVC>,/UPy_LIMITED_API,-UPy_LIMITED_API>")
add_custom_command(OUTPUT ${PyEngine_SOURCES}
    COMMAND $<TARGET_FILE:${SHIBOKEN_EXE}> ${shiboken_args}
    COMMAND ${NATRON_PYTHON_COMMAND} ../tools/utils/sourceCleanup.py typesystem_engine.xml ${PYENGINE_OUT}
    DEPENDS ${SHIBOKEN_EXE} ${PYENGINE_HEADER} ${NatronEngine_HEADERS} ../tools/utils/sourceCleanup.py typesystem_engine.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})




list(APPEND NatronEngine_SOURCES
    ${NatronEngine_SOURCES}
    ${PyEngine_SOURCES}
    ../Global/FStreamsSupport.cpp
    ../Global/ProcInfo.cpp
    ../Global/PythonUtils.cpp
    ../Global/StrUtils.cpp
    ../libs/SequenceParsing/SequenceParsing.cpp
)
add_library(NatronEngine STATIC ${NatronEngine_HEADERS} ${NatronEngine_SOURCES})
target_link_libraries(NatronEngine
    PUBLIC
        HostSupport
        Boost::headers
        Boost::serialization
        PkgConfig::Cairo
    PRIVATE
        NatronGpu
        NatronReconstruction
        ${SHIBOKEN_LIB}
        ${PYSIDE_LIB}
        Qt::Core
        Qt::Concurrent
        Qt::Network
        Python3::Module
        ceres
        hoedown
        mv
        openMVG
        tess
)

if(NATRON_ENABLE_USD)
    target_link_libraries(NatronEngine PRIVATE NatronReconstructionUsd)
endif()

if(WIN32)
    #System library is required on windows to map network share names from drive letters
    target_link_libraries(NatronEngine PRIVATE mpr)
endif()

target_include_directories(NatronEngine
    PRIVATE
        ${PYSIDE_INCLUDE_DIRS}/QtCore
        .
        ..
        ../Global
        ../libs/SequenceParsing
)
target_compile_definitions(NatronEngine
    PRIVATE
        NATRON_CUSTOM_BUILD_USER_TOKEN="${BUILD_USER_NAME}"
        NATRON_BUILD_NUMBER=0
        QT_NO_KEYWORDS
)
