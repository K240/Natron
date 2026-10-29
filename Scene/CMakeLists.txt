add_library(NatronReconstruction STATIC CameraSolve.cpp FeatureTracking.cpp CameraIntrinsics.cpp SceneFrame.cpp)
target_compile_features(NatronReconstruction PUBLIC cxx_std_17)
target_include_directories(NatronReconstruction PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(NatronReconstruction PRIVATE mv_reconstruction)
set_target_properties(NatronReconstruction PROPERTIES
    POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTORCC OFF)

if(NATRON_ENABLE_USD)
    find_package(pxr CONFIG REQUIRED)
    add_library(NatronReconstructionUsd STATIC ReconstructionUsd.cpp)
    target_compile_features(NatronReconstructionUsd PUBLIC cxx_std_17)
    target_include_directories(NatronReconstructionUsd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(NatronReconstructionUsd PRIVATE NatronReconstruction usd usdGeom)
    set_target_properties(NatronReconstructionUsd PROPERTIES
        POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTORCC OFF)
endif()
