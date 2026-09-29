cmake_minimum_required(VERSION 3.24)
project(PimaxGazeCalibRecorder VERSION 0.1.0 LANGUAGES CXX)

# OpenXR headers only. Left empty, the pinned Khronos OpenXR-SDK release is
# downloaded at configure time. The loader is not linked: the app loads
# openxr_loader.dll beside the executable at run time and pulls every entry
# point through xrGetInstanceProcAddr.
set(XRCAL_OPENXR_SDK_ROOT ""
    CACHE PATH "OpenXR-SDK folder containing include/openxr/openxr.h (empty: download it)")

if(XRCAL_OPENXR_SDK_ROOT)
    set(XRCAL_OPENXR_INCLUDE "${XRCAL_OPENXR_SDK_ROOT}/include")
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
    set(XRCAL_OPENXR_INCLUDE "${openxr_sdk_SOURCE_DIR}/include")
endif()

set(XRCAL_OPENXR_LOADER_DLL
    ""
    CACHE FILEPATH "openxr_loader.dll to copy next to the executable after the build")

add_executable(calib_recorder main.cpp)
target_include_directories(calib_recorder PRIVATE "${XRCAL_OPENXR_INCLUDE}")
target_compile_features(calib_recorder PRIVATE cxx_std_20)
target_link_libraries(calib_recorder PRIVATE d3d11 dxgi d3dcompiler)

if(MSVC)
    target_compile_options(calib_recorder PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(calib_recorder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

if(XRCAL_OPENXR_LOADER_DLL)
    add_custom_command(TARGET calib_recorder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XRCAL_OPENXR_LOADER_DLL}" "$<TARGET_FILE_DIR:calib_recorder>/openxr_loader.dll"
        COMMENT "Copying openxr_loader.dll next to calib_recorder.exe")
endif()
