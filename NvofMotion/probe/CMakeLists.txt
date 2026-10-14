# Standalone NvOF stock-recipe probe (console exe, static runtime).
# Built in CI alongside the addon; run on the rig, paste console output.
cmake_minimum_required(VERSION 3.24)
project(nvof_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CUDAToolkit REQUIRED)

add_executable(nvof_probe nvof_probe.cpp)
target_include_directories(nvof_probe PRIVATE "${NVOF_SDK}")
target_link_libraries(nvof_probe PRIVATE CUDA::cuda_driver)
target_compile_definitions(nvof_probe PRIVATE UNICODE _UNICODE)
