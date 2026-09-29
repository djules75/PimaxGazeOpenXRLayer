cmake_minimum_required(VERSION 3.24)
project(PimaxGazeLayer VERSION 0.1.0 LANGUAGES CXX)

# OpenXR headers only; nothing from the SDK is built or linked. Left empty,
# the pinned Khronos OpenXR-SDK release is downloaded at configure time.
set(PGZ_OPENXR_SDK_ROOT ""
    CACHE PATH "OpenXR-SDK folder containing include/openxr/openxr.h (empty: download it)")

if(PGZ_OPENXR_SDK_ROOT)
    set(PGZ_OPENXR_INCLUDE "${PGZ_OPENXR_SDK_ROOT}/include")
else()
    include(FetchContent)
    # SOURCE_SUBDIR names a folder without a CMakeLists.txt, so the SDK is
    # downloaded but not added to the build.
    FetchContent_Declare(openxr_sdk
        URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz
        URL_HASH SHA256=d5e2773d282642e6e250bd10266f316fc3ff3b8602c83db009a1f434205998e6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR headers-only)
    FetchContent_MakeAvailable(openxr_sdk)
    set(PGZ_OPENXR_INCLUDE "${openxr_sdk_SOURCE_DIR}/include")
endif()

add_library(PimaxGazeLayer SHARED gaze_layer.cpp)
target_include_directories(PimaxGazeLayer PRIVATE "${PGZ_OPENXR_INCLUDE}")
target_compile_features(PimaxGazeLayer PRIVATE cxx_std_20)
set_target_properties(PimaxGazeLayer PROPERTIES PREFIX "" OUTPUT_NAME "PimaxGazeLayer")

if(MSVC)
    target_compile_options(PimaxGazeLayer PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(PimaxGazeLayer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(TARGET PimaxGazeLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/XR_APILAYER_PIMAXGAZE_eye_gaze.json"
            "$<TARGET_FILE_DIR:PimaxGazeLayer>/XR_APILAYER_PIMAXGAZE_eye_gaze.json"
    COMMENT "Copying the layer manifest next to the DLL")
