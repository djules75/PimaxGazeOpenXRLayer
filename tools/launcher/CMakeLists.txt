cmake_minimum_required(VERSION 3.24)
project(PimaxGazeLauncher VERSION 0.1.0 LANGUAGES CXX)

# PimaxGaze.exe: registers the layer and runs the gaze service. Ship it in
# dist/layer/, next to XR_APILAYER_PIMAXGAZE_eye_gaze.json.
add_executable(PimaxGaze main.cpp)
target_compile_features(PimaxGaze PRIVATE cxx_std_20)
target_compile_definitions(PimaxGaze PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(PimaxGaze PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(PimaxGaze PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    set_property(TARGET PimaxGaze PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
