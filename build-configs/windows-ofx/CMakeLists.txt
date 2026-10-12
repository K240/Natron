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
    # Public C ABI headers and the SDK adapter for native curve backgrounds.
    get_filename_component(natron_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    target_include_directories(Misc PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native")
    get_target_property(misc_sources Misc SOURCES)
    list(FILTER misc_sources EXCLUDE REGEX "/(SupportExt/(glad/.*|ofxsOGL(TextRenderer|FontData|Utilities)\\.cpp)|Shadertoy/ShadertoyRender(GL|Mesa)\\.cpp|Test/TestOpenGL[^/]*\\.cpp)$")
    set_property(TARGET Misc PROPERTY SOURCES "${misc_sources}")
    target_sources(Misc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/ShadertoyVulkan.cpp")
    if(WIN32)
        target_sources(Misc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/MiscVulkan.def")
    endif()
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/native/ShadertoyVulkan.cpp"
        "${OFX_SOURCE_ROOT}/openfx-misc/Shadertoy/Shadertoy.cpp"
        TARGET_DIRECTORY Misc PROPERTIES INCLUDE_DIRECTORIES
        "${natron_source_root};${OFX_SOURCE_ROOT}/openfx-misc/Shadertoy")
    set_source_files_properties(
        "${OFX_SOURCE_ROOT}/openfx-misc/ColorLookup/ColorLookup.cpp"
        "${OFX_SOURCE_ROOT}/openfx-misc/HueCorrect/HueCorrect.cpp"
        "${OFX_SOURCE_ROOT}/openfx-misc/HueCorrect/HueCorrect1.cpp"
        TARGET_DIRECTORY Misc
        PROPERTIES INCLUDE_DIRECTORIES "${natron_source_root};${CMAKE_CURRENT_SOURCE_DIR}/native")
elseif(OFX_SET STREQUAL "arena")
    find_package(PkgConfig REQUIRED)
    set(AUDIO ON CACHE BOOL "Build AudioCurve")
    set(READMISC_LEGACY OFF CACHE BOOL "Retire legacy ImageMagick formats")
    set(OLD_POPPLER OFF CACHE BOOL "Use current Poppler API")
    add_subdirectory("${OFX_SOURCE_ROOT}/openfx-arena" arena)
    pkg_check_modules(ARENA_EXTRA REQUIRED IMPORTED_TARGET pangoft2 poppler OpenColorIO)
    target_link_libraries(Arena PkgConfig::ARENA_EXTRA shlwapi)
    target_compile_definitions(Arena PRIVATE OFX_IO_USING_OCIO)
    get_filename_component(natron_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    target_include_directories(Arena PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native")
    get_target_property(arena_sources Arena SOURCES)
    list(FILTER arena_sources EXCLUDE REGEX "(^|/)SupportExt/(glad/.*|ofxsOGL(TextRenderer|FontData|Utilities)\\.cpp)$")
    set_property(TARGET Arena PROPERTY SOURCES "${arena_sources}")
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
elseif(OFX_SET STREQUAL "io")
    find_package(OpenColorIO 2.5 CONFIG REQUIRED)
    find_package(OpenImageIO CONFIG REQUIRED)
    add_subdirectory("${OFX_SOURCE_ROOT}/openfx-io" io)
    # OCIO color processing uses the native suite; retire its GL shader backend.
    get_target_property(io_sources IO SOURCES)
    list(FILTER io_sources EXCLUDE REGEX "/IOSupport/(GenericOCIOOpenGL|glsl)\\.cpp$")
    list(FILTER io_sources EXCLUDE REGEX "(^|/)SupportExt/(glad/.*|ofxsOGL(TextRenderer|FontData|Utilities)\\.cpp)$")
    set_property(TARGET IO PROPERTY SOURCES "${io_sources}")
    target_compile_definitions(IO PRIVATE OFX_IO_NATIVE_OCIO)
    get_filename_component(natron_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    target_sources(IO PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/native/OcioVulkan.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/OcioVulkanAdapter.cpp")
    if(WIN32)
        target_sources(IO PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/IOVulkan.def")
    endif()
    target_include_directories(IO PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native")
    target_link_libraries(IO PRIVATE OpenImageIO::OpenImageIO)
    add_executable(IOOverlayDrawingTest "${CMAKE_CURRENT_SOURCE_DIR}/native/tests/OverlayDrawing.cpp")
    target_include_directories(IOOverlayDrawingTest PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${OFX_SOURCE_ROOT}/openfx-io/openfx/include" "${OFX_SOURCE_ROOT}/openfx-io/openfx/Support/include")
    set_target_properties(IOOverlayDrawingTest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_executable(OcioVulkanFailureTest "${CMAKE_CURRENT_SOURCE_DIR}/native/tests/OcioFailures.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/OcioVulkan.cpp")
    target_include_directories(OcioVulkanFailureTest PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${OFX_SOURCE_ROOT}/openfx-io/openfx/include")
    target_link_libraries(OcioVulkanFailureTest PRIVATE OpenColorIO::OpenColorIO)
    add_library(OcioVulkanSuiteSmoke MODULE "${natron_source_root}/Tests/OcioVulkanSuitePlugin.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/OcioVulkan.cpp")
    target_include_directories(OcioVulkanSuiteSmoke PRIVATE "${natron_source_root}" "${CMAKE_CURRENT_SOURCE_DIR}/native"
        "${OFX_SOURCE_ROOT}/openfx-io/openfx/include")
    target_link_libraries(OcioVulkanSuiteSmoke PRIVATE OpenColorIO::OpenColorIO)
    set_target_properties(OcioVulkanSuiteSmoke PROPERTIES PREFIX "" SUFFIX ".ofx"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../../ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../../ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64")
    set(ofx_targets IO)
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
if(TARGET IO)
    set_target_properties(IO PROPERTIES CXX_STANDARD 20)
endif()
