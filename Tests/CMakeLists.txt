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

set(Tests_HEADERS BaseTest.h)
set(Tests_SOURCES
    google-test/src/gtest-all.cc
    google-mock/src/gmock-all.cc
    BaseTest.cpp
    Curve_Test.cpp
    FileSystemModel_Test.cpp
    Hash64_Test.cpp
    Image_Test.cpp
    KnobFile_Test.cpp
    Lut_Test.cpp
    OfxDrawSuite_Test.cpp
    OverlayDrawList_Test.cpp
    Tracker_Test.cpp
    wmain.cpp
)
add_executable(Tests ${Tests_HEADERS} ${Tests_SOURCES})
target_link_libraries(Tests
    PRIVATE
        NatronEngine
        Qt::Core
        Python3::Python
        openMVG
)
target_include_directories(Tests
    PRIVATE
        ..
        ../libs/SequenceParsing
        google-test/include
        google-test
        google-mock/include
        google-mock
)
add_test(NAME Tests COMMAND Tests)

# Hardware-dependent Engine tests run explicitly through the Vulkan runner.
add_executable(NatronVulkanImageStorage VulkanImageStorage.cpp)
target_compile_features(NatronVulkanImageStorage PRIVATE cxx_std_17)
target_link_libraries(NatronVulkanImageStorage PRIVATE NatronEngine NatronGpu Qt::Core Python3::Python)
add_executable(NatronVulkanImageConvert VulkanImageConvert.cpp)
target_compile_features(NatronVulkanImageConvert PRIVATE cxx_std_17)
target_link_libraries(NatronVulkanImageConvert PRIVATE NatronEngine NatronGpu Qt::Core Python3::Python)
add_executable(NatronVulkanClipBorrow VulkanClipBorrow.cpp)
target_compile_features(NatronVulkanClipBorrow PRIVATE cxx_std_17)
target_link_libraries(NatronVulkanClipBorrow PRIVATE NatronEngine NatronGpu Qt::Core Python3::Python)

# Exercise the public OFX DrawSuite ABI independently of Natron's C++ API.
add_library(DrawSuiteSmoke MODULE DrawSuitePlugin.cpp)
target_compile_features(DrawSuiteSmoke PRIVATE cxx_std_17)
target_include_directories(DrawSuiteSmoke PRIVATE .. ../libs/OpenFX/include)
if(WIN32)
    set(_draw_suite_platform Win64)
elseif(APPLE)
    set(_draw_suite_platform MacOS)
else()
    set(_draw_suite_platform Linux-x86-64)
endif()
set_target_properties(DrawSuiteSmoke PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/DrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/DrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
)

# Native custom-parameter extension, independently loaded through the OFX C ABI.
add_library(LegacyInteractSmoke MODULE LegacyInteractPlugin.cpp)
target_compile_features(LegacyInteractSmoke PRIVATE cxx_std_17)
target_include_directories(LegacyInteractSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(LegacyInteractSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/LegacyInteractSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/LegacyInteractSmoke.ofx.bundle/Contents/${_draw_suite_platform}")

add_library(RetiredRenderSmoke MODULE RetiredRenderPlugin.cpp)
target_compile_features(RetiredRenderSmoke PRIVATE cxx_std_17)
target_include_directories(RetiredRenderSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(RetiredRenderSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/${_draw_suite_platform}")

add_library(ParamDrawSuiteSmoke MODULE ParamDrawSuitePlugin.cpp)
target_compile_features(ParamDrawSuiteSmoke PRIVATE cxx_std_17)
target_include_directories(ParamDrawSuiteSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(ParamDrawSuiteSmoke PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/ParamDrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/ParamDrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
)

add_library(CurveDrawSuiteSmoke MODULE CurveDrawSuitePlugin.cpp)
target_compile_features(CurveDrawSuiteSmoke PRIVATE cxx_std_17)
target_include_directories(CurveDrawSuiteSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(CurveDrawSuiteSmoke PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/CurveDrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/CurveDrawSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
)

# Exercise Engine Vulkan compute through an independently compiled OFX C ABI.
add_library(VulkanImageSuiteSmoke MODULE VulkanImageSuitePlugin.cpp)
target_compile_features(VulkanImageSuiteSmoke PRIVATE cxx_std_17)
target_include_directories(VulkanImageSuiteSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(VulkanImageSuiteSmoke PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
)

# The raster display contract runs without creating a Vulkan device or loader.
add_library(VulkanMemoryFailureSmoke MODULE VulkanMemoryFailurePlugin.cpp)
target_compile_features(VulkanMemoryFailureSmoke PRIVATE cxx_std_17)
target_include_directories(VulkanMemoryFailureSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(VulkanMemoryFailureSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/${_draw_suite_platform}")

add_executable(NatronRasterViewer ../Gpu/tests/RasterViewer.cpp)
add_library(VulkanPressureSmoke MODULE VulkanPressurePlugin.cpp)
add_library(VulkanBenchmarkSmoke MODULE VulkanBenchmarkPlugin.cpp)
target_compile_features(VulkanBenchmarkSmoke PRIVATE cxx_std_17)
target_include_directories(VulkanBenchmarkSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(VulkanBenchmarkSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/${_draw_suite_platform}")
target_compile_features(VulkanPressureSmoke PRIVATE cxx_std_17)
target_include_directories(VulkanPressureSmoke PRIVATE .. ../libs/OpenFX/include)
set_target_properties(VulkanPressureSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/${_draw_suite_platform}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/${_draw_suite_platform}")

target_link_libraries(NatronRasterViewer PRIVATE NatronGpu)
add_test(NAME RasterViewerNoGpu COMMAND NatronRasterViewer)
set_tests_properties(RasterViewerNoGpu PROPERTIES TIMEOUT 30)

# Explicit hardware comparison, kept out of the CPU-only default CTest suite.
add_executable(NatronRasterGpuCompare ../Gpu/tests/RasterGpuCompare.cpp)
target_link_libraries(NatronRasterGpuCompare PRIVATE NatronGpu)
