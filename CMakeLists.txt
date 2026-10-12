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

cmake_minimum_required(VERSION 3.24)

project(Natron
    VERSION "2.6.0"
    DESCRIPTION "Open Source Compositing Software"
    HOMEPAGE_URL "https://natrongithub.github.io/"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Git QUIET)
set(NATRON_GIT_BRANCH "unknown")
set(NATRON_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE NATRON_GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE NATRON_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
configure_file(Global/GitVersion.h.in generated/Global/GitVersion.h @ONLY)
include_directories(BEFORE "${CMAKE_BINARY_DIR}/generated")

option(NATRON_SYSTEM_LIBS "use system versions of dependencies instead of bundled ones" OFF)
option(NATRON_BUILD_TESTS "build the Natron test suite" ON)
option(NATRON_BUILD_GPU_TESTS "build Vulkan tests requiring a GPU and a display" OFF)

set(IS_DEBUG_BUILD OFF)
if(CMAKE_BUILD_TYPE MATCHES "^(debug|Debug|DEBUG)$")
    set(IS_DEBUG_BUILD ON)
    add_definitions(-DDEBUG)

    if(WIN32)
        # Debug builds need minimal optimizations to avoid excessive link times and link errors related to Eigen.
        add_compile_options($<IF:$<CXX_COMPILER_ID:MSVC>,/O1,-O1>)
    endif()
else()
    add_definitions(-DQT_NO_DEBUG_OUTPUT)
endif()

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build." FORCE)
    message(STATUS "Setting build type to '${CMAKE_BUILD_TYPE}' as none was specified.")
endif()

find_package(Boost COMPONENTS headers serialization CONFIG)
if(WIN32)
    # Use the explicitly selected Python interpreter instead of registry installations.
    set(Python3_FIND_REGISTRY NEVER)
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development)

find_program(NATRON_UV_EXECUTABLE uv REQUIRED)
set(NATRON_PYTHON_COMMAND "${NATRON_UV_EXECUTABLE}" run --no-project
    --python "${Python3_EXECUTABLE}" python)

if(IS_DEBUG_BUILD AND WIN32)
    # Explicitly setting SHIBOKEN_PYTHON_LIBRARIES variable to avoid PYTHON_DEBUG_LIBRARY-NOTFOUND
    # link errors on Windows debug builds.
    set(SHIBOKEN_PYTHON_LIBRARIES ${Python3_LIBRARIES})
endif()

find_package(Qt6 6.6 CONFIG REQUIRED COMPONENTS Core Gui Network Widgets Concurrent)
find_package(Shiboken6 6.6 CONFIG REQUIRED COMPONENTS libshiboken6)
find_package(Shiboken6Tools 6.6 CONFIG REQUIRED COMPONENTS shiboken6)
find_package(PySide6 6.6 CONFIG REQUIRED COMPONENTS pyside6)
set(QT_VERSION_MAJOR 6)
set(PYSIDE_VERSION_MAJOR 6)
get_target_property(PYSIDE_INCLUDE_DIRS PySide6::pyside6 INTERFACE_INCLUDE_DIRECTORIES)
set(QTCORE_INCLUDE_DIRS ${Qt6Core_INCLUDE_DIRS})
set(QTGUI_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS})
set(QTWIDGETS_INCLUDE_DIRS ${Qt6Widgets_INCLUDE_DIRS})
set(SHIBOKEN_EXE Shiboken6::shiboken6)
set(SHIBOKEN_LIB Shiboken6::libshiboken)
set(PYSIDE_LIB PySide6::pyside6)

if(IS_DEBUG_BUILD AND WIN32)
    # Remove NDEBUG from Shiboken INTERFACE_COMPILE_DEFINITIONS so it is not inherited in debug builds.
    get_property(SHIBOKEN_DEFS TARGET ${SHIBOKEN_LIB} PROPERTY INTERFACE_COMPILE_DEFINITIONS)
    list(REMOVE_ITEM SHIBOKEN_DEFS NDEBUG)
    set_property(TARGET ${SHIBOKEN_LIB} PROPERTY INTERFACE_COMPILE_DEFINITIONS SHIBOKEN_DEFS)
endif()

#Since in Natron and OpenFX all strings are supposed UTF-8 and that the constructor
#for QString(const char*) assumes ASCII strings, we may run into troubles
add_compile_definitions(QT_NO_CAST_FROM_ASCII)

if (MSVC)
    # Allow __cplusplus to properly reflect the c++ standard version.
    add_compile_options(/Zc:__cplusplus /utf-8 /bigobj)
    add_compile_definitions(__STDC_LIMIT_MACROS WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
else()
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-prototypes" )
    add_compile_options(-Wall -Wextra -Wmissing-declarations -Wno-multichar -Winit-self -Wno-long-long
                        -Wvla -Wdate-time -Wno-format-overflow -Wno-deprecated-copy)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wshift-overflow=2 -Wduplicated-cond -Wno-stringop-overflow)
    endif()
endif()

if(NATRON_SYSTEM_LIBS)
    find_package(glog)
    find_package(Ceres)
endif()

if(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    enable_language(RC)
    add_compile_definitions(WINDOWS WIN32 _UNICODE UNICODE NOMINMAX QHTTP_SERVER_STATIC)
    add_compile_definitions(COMPILED_FROM_DSP XML_STATIC) # for expat
    if (NOT MSVC)
        add_compile_options(-municode)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-mwindows -mthreads)
        endif()
        add_link_options(-mwindows -municode)
        set(CMAKE_RC_COMPILER_INIT windres)
        set(CMAKE_RC_COMPILE_OBJECT
            "<CMAKE_RC_COMPILER> <FLAGS> -O coff <DEFINES> -i <SOURCE> -o <OBJECT>")
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

enable_testing()

add_subdirectory(libs)

add_subdirectory(Gpu)

add_subdirectory(HostSupport)

add_subdirectory(Engine)

add_subdirectory(PythonBin)

add_subdirectory(Renderer)

add_subdirectory(Gui)

if(NATRON_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

add_subdirectory(App)
