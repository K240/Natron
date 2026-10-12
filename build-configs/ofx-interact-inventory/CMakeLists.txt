cmake_minimum_required(VERSION 3.24)
project(NatronOfxInteractInventory LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "The descriptor inventory currently targets the Windows completion scope.")
endif()
get_filename_component(natron_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NATRON_HOST_BUILD_DIR "${natron_root}/build/windows-qt6" CACHE PATH "Built Natron HostSupport library")
find_package(Qt6 6.6 REQUIRED COMPONENTS Core)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(EXPAT REQUIRED)
add_library(NatronInventoryHost STATIC IMPORTED)
set_target_properties(NatronInventoryHost PROPERTIES IMPORTED_LOCATION "${NATRON_HOST_BUILD_DIR}/HostSupport/HostSupport.lib")
add_executable(OfxInteractInventory "${natron_root}/tools/ofx-interact-inventory.cpp")
target_compile_features(OfxInteractInventory PRIVATE cxx_std_17)
target_compile_options(OfxInteractInventory PRIVATE /permissive- /Zc:__cplusplus)
target_compile_definitions(OfxInteractInventory PRIVATE WINDOWS WIN32 UNICODE _UNICODE
    OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_VEGAS OFX_SUPPORTS_PARAMETRIC
    OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_RESOLVE OFX_SUPPORTS_MULTITHREAD OFX_SUPPORTS_DIALOG)
target_include_directories(OfxInteractInventory PRIVATE "${natron_root}"
    "${natron_root}/libs/OpenFX/include" "${natron_root}/libs/OpenFX_extensions"
    "${natron_root}/libs/OpenFX/HostSupport/include")
target_link_libraries(OfxInteractInventory PRIVATE NatronInventoryHost EXPAT::EXPAT Qt6::Core Python3::Python
    shell32 ole32 advapi32)
