cmake_minimum_required(VERSION 3.24)
project(NatronOFX LANGUAGES CXX)

set(OFX_SOURCE_ROOT "" CACHE PATH "Directory containing the pinned OpenFX repositories")
set(OFX_SET "misc" CACHE STRING "OpenFX set: misc, arena, or gmic")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(OFX_SET STREQUAL "misc")
    add_subdirectory("${OFX_SOURCE_ROOT}/openfx-misc" misc)
    target_sources(Misc PRIVATE "${OFX_SOURCE_ROOT}/openfx-misc/Misc/randomGenerator.cpp"
        "${OFX_SOURCE_ROOT}/openfx-misc/Test/TestRender.cpp")
    set(ofx_targets Misc CImg)
elseif(OFX_SET STREQUAL "arena")
    find_package(PkgConfig REQUIRED)
    set(AUDIO ON CACHE BOOL "Build AudioCurve")
    set(READMISC_LEGACY OFF CACHE BOOL "Retire legacy ImageMagick formats")
    set(OLD_POPPLER OFF CACHE BOOL "Use current Poppler API")
    add_subdirectory("${OFX_SOURCE_ROOT}/openfx-arena" arena)
    pkg_check_modules(ARENA_EXTRA REQUIRED IMPORTED_TARGET pangoft2 poppler OpenColorIO)
    target_link_libraries(Arena PkgConfig::ARENA_EXTRA shlwapi)
    target_compile_definitions(Arena PRIVATE OFX_IO_USING_OCIO)
    set(ofx_targets Arena)
elseif(OFX_SET STREQUAL "gmic")
    set(src "${OFX_SOURCE_ROOT}/openfx-gmic")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GMIC_DEPS REQUIRED IMPORTED_TARGET fftw3 zlib libcurl libpng)
    find_library(FFTW_THREADS NAMES fftw3_threads REQUIRED)
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    add_library(GMIC SHARED
        "${src}/gmic.cpp" "${src}/gmic_libc.cpp" "${src}/gmic_plugin.cpp"
        "${src}/Helpers/gmic_parser.cpp" "${src}/Helpers/RFX_FileUtils.cpp"
        "${src}/Helpers/RFX_StringUtils.cpp")
    if(WIN32)
        target_sources(GMIC PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gmic.def")
    endif()
    target_include_directories(GMIC PRIVATE "${src}" "${src}/Helpers"
        "${src}/openfx/include" "${src}/openfx/Examples/include")
    target_compile_definitions(GMIC PRIVATE OFX_PLUGIN OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_NATRON
        NOMINMAX gmic_build gmic_is_parallel cimg_use_abort cimg_display=0
        cimg_appname="gmic" cimg_namespace_suffix=openfx_gmic cimg_verbosity=0
        cimg_library=cimg_library_openfx_gmic cimg_use_fftw3 cimg_use_zlib
        cimg_use_curl cimg_use_png cimg_use_openmp)
    target_link_libraries(GMIC PRIVATE PkgConfig::GMIC_DEPS ${FFTW_THREADS} OpenMP::OpenMP_CXX)
    set_target_properties(GMIC PROPERTIES PREFIX "" SUFFIX ".ofx")
    install(TARGETS GMIC RUNTIME DESTINATION GMIC.ofx.bundle/Contents/Win64
        LIBRARY DESTINATION GMIC.ofx.bundle/Contents/Win64)
    install(FILES "${src}/GMIC_OFX/GMIC.png" DESTINATION GMIC.ofx.bundle/Contents/Resources)
    set(ofx_targets GMIC)
else()
    message(FATAL_ERROR "Unknown OFX_SET: ${OFX_SET}")
endif()

foreach(target IN LISTS ofx_targets)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    if(MINGW)
        target_compile_options(${target} PRIVATE -Wa,-mbig-obj)
        # Keep finite-value checks used by image processing algorithms intact.
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-fno-fast-math>)
    endif()
endforeach()

# Current Poppler headers use std::span.
if(TARGET Arena)
    set_target_properties(Arena PROPERTIES CXX_STANDARD 20)
endif()
