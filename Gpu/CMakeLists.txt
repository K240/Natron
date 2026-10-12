set(NATRON_VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK containing Volk, VMA and shader tools")
if(DEFINED NATRON_GPU_RESOLVED_SDK_ROOT AND
   NOT "${NATRON_GPU_RESOLVED_SDK_ROOT}" STREQUAL "${NATRON_VULKAN_SDK_ROOT}")
    unset(NATRON_VOLK_INCLUDE_DIR CACHE)
    unset(NATRON_VMA_INCLUDE_DIR CACHE)
    unset(NATRON_SPIRV_VALIDATOR CACHE)
endif()
set(NATRON_GPU_RESOLVED_SDK_ROOT "${NATRON_VULKAN_SDK_ROOT}" CACHE INTERNAL "Last resolved Vulkan helper SDK" FORCE)
find_package(Vulkan 1.4.363 REQUIRED COMPONENTS glslangValidator shaderc_combined)
find_path(NATRON_VOLK_INCLUDE_DIR volk.h HINTS "${NATRON_VULKAN_SDK_ROOT}" PATH_SUFFIXES Include/Volk include/volk REQUIRED)
find_path(NATRON_VMA_INCLUDE_DIR vk_mem_alloc.h HINTS "${NATRON_VULKAN_SDK_ROOT}" PATH_SUFFIXES Include/vma include/vma REQUIRED)
find_program(NATRON_SPIRV_VALIDATOR spirv-val HINTS "${NATRON_VULKAN_SDK_ROOT}/Bin" REQUIRED)
set(shader_spv "${CMAKE_CURRENT_BINARY_DIR}/transform.comp.spv")
set(shader_header "${CMAKE_CURRENT_BINARY_DIR}/TransformShader.h")
add_custom_command(OUTPUT "${shader_header}"
    COMMAND Vulkan::glslangValidator -V --target-env vulkan1.3 -S comp
        -o "${shader_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/transform.comp"
    COMMAND "${NATRON_SPIRV_VALIDATOR}" --target-env vulkan1.3 "${shader_spv}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${shader_spv}" "-DOUTPUT=${shader_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShader.cmake"
    DEPENDS shaders/transform.comp EmbedShader.cmake VERBATIM)
set(viewer_spv "${CMAKE_CURRENT_BINARY_DIR}/viewer.comp.spv")
set(viewer_header "${CMAKE_CURRENT_BINARY_DIR}/ViewerShader.h")
add_custom_command(OUTPUT "${viewer_header}"
    COMMAND Vulkan::glslangValidator -V --target-env vulkan1.3 -S comp
        -o "${viewer_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/viewer.comp"
    COMMAND "${NATRON_SPIRV_VALIDATOR}" --target-env vulkan1.3 "${viewer_spv}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${viewer_spv}" "-DOUTPUT=${viewer_header}" -DSYMBOL=ViewerShader
        -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShader.cmake"
    DEPENDS shaders/viewer.comp EmbedShader.cmake VERBATIM)
set(operations_spv "${CMAKE_CURRENT_BINARY_DIR}/image-operations.comp.spv")
set(operations_header "${CMAKE_CURRENT_BINARY_DIR}/ImageOperationsShader.h")
add_custom_command(OUTPUT "${operations_header}"
    COMMAND Vulkan::glslangValidator -V --target-env vulkan1.3 -S comp
        -o "${operations_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/image-operations.comp"
    COMMAND "${NATRON_SPIRV_VALIDATOR}" --target-env vulkan1.3 "${operations_spv}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${operations_spv}" "-DOUTPUT=${operations_header}" -DSYMBOL=ImageOperationsShader
        -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShader.cmake"
    DEPENDS shaders/image-operations.comp EmbedShader.cmake VERBATIM)
set(display_spv "${CMAKE_CURRENT_BINARY_DIR}/viewer-display.comp.spv")
set(display_header "${CMAKE_CURRENT_BINARY_DIR}/ViewerDisplayShader.h")
add_custom_command(OUTPUT "${display_header}"
    COMMAND Vulkan::glslangValidator -V --target-env vulkan1.3 -S comp
        -o "${display_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/viewer-display.comp"
    COMMAND "${NATRON_SPIRV_VALIDATOR}" --target-env vulkan1.3 "${display_spv}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${display_spv}" "-DOUTPUT=${display_header}" -DSYMBOL=ViewerDisplayShader
        -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShader.cmake"
    DEPENDS shaders/viewer-display.comp EmbedShader.cmake VERBATIM)
add_library(NatronGpu STATIC VulkanDevice.cpp VulkanMemory.cpp ViewerFrame.cpp ViewerRenderer.cpp RasterViewerRenderer.cpp ViewerDisplayConversion.cpp ImageOperations.cpp FragmentProgram.cpp LutTexture.cpp
    "${shader_header}" "${viewer_header}" "${operations_header}" "${display_header}")
target_compile_features(NatronGpu PUBLIC cxx_std_17)
target_compile_definitions(NatronGpu PUBLIC VK_NO_PROTOTYPES VOLK_NAMESPACE)
target_include_directories(NatronGpu
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/.." "${NATRON_VOLK_INCLUDE_DIR}"
    PRIVATE "${NATRON_VMA_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(NatronGpu PUBLIC Vulkan::Headers PRIVATE Vulkan::shaderc_combined "${CMAKE_DL_LIBS}")
set_target_properties(NatronGpu PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTORCC OFF)
if(MSVC)
    target_compile_options(NatronGpu PRIVATE /utf-8 /Zc:__cplusplus)
    set_source_files_properties(VulkanMemory.cpp PROPERTIES COMPILE_OPTIONS /w)
else()
    set_source_files_properties(VulkanMemory.cpp PROPERTIES COMPILE_OPTIONS -w)
endif()
add_library(NatronGpuQt STATIC Qt/VulkanCanvas.cpp)
target_link_libraries(NatronGpuQt PUBLIC NatronGpu Qt6::Gui Qt6::Widgets)
target_compile_features(NatronGpuQt PUBLIC cxx_std_17)
set_target_properties(NatronGpuQt PROPERTIES AUTOMOC OFF AUTORCC OFF)
if(NATRON_BUILD_GPU_TESTS)
    add_subdirectory(tests)
endif()
