cmake_minimum_required(VERSION 3.15)
project(NvofMotion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths ───────────────────────────────────────────────────────────────────
set(RESHADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../reshade-main" CACHE PATH
    "Root of the crosire/reshade checkout (must contain include/ and deps/)")
# Root of the NVIDIAOpticalFlowSDK checkout (nvOpticalFlowCommon.h and
# nvOpticalFlowCuda.h at its root). Phase 1b+ requires it.
set(NVOF_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../nvof-sdk" CACHE PATH
    "Root of the NVIDIAOpticalFlowSDK checkout")
# Root of an installed CUDA toolkit (provides cuda.h for types and nvcc for
# the PTX kernel - the add-on binds nvcuda.dll explicitly at runtime,
# no cuda.lib linkage).
set(CUDA_TOOLKIT_ROOT "$ENV{CUDA_PATH}" CACHE PATH
    "CUDA toolkit root (cuda.h lives in <root>/include)")

set(RESHADE_INCLUDE "${RESHADE_ROOT}/include")
set(IMGUI_INCLUDE   "${RESHADE_ROOT}/deps/imgui")

if(NOT EXISTS "${RESHADE_INCLUDE}/reshade.hpp")
    message(FATAL_ERROR
        "Cannot find reshade.hpp – set RESHADE_ROOT to the reshade-main directory.\n"
        "  Current RESHADE_ROOT = ${RESHADE_ROOT}")
endif()
if(NOT EXISTS "${NVOF_SDK}/nvOpticalFlowCommon.h")
    message(FATAL_ERROR
        "Cannot find nvOpticalFlowCommon.h – set NVOF_SDK to the NVIDIAOpticalFlowSDK directory.\n"
        "  Current NVOF_SDK = ${NVOF_SDK}")
endif()
if(NOT EXISTS "${CUDA_TOOLKIT_ROOT}/include/cuda.h")
    message(FATAL_ERROR
        "Cannot find cuda.h – set CUDA_TOOLKIT_ROOT to an installed CUDA toolkit.\n"
        "  Current CUDA_TOOLKIT_ROOT = ${CUDA_TOOLKIT_ROOT}")
endif()

# ── PTX kernel (flow_convert.cu -> flow_ptx.h) ──────────────────────────────
find_program(NVCC_EXECUTABLE nvcc
    HINTS "${CUDA_TOOLKIT_ROOT}/bin"
    NO_DEFAULT_PATH)
if(NOT NVCC_EXECUTABLE)
    find_program(NVCC_EXECUTABLE nvcc)
endif()
if(NOT NVCC_EXECUTABLE)
    message(FATAL_ERROR "nvcc not found – needed to compile flow_convert.cu to PTX.")
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/flow_convert.ptx"
    COMMAND "${NVCC_EXECUTABLE}" -ptx
            "${CMAKE_CURRENT_SOURCE_DIR}/flow_convert.cu"
            -o "${CMAKE_CURRENT_BINARY_DIR}/flow_convert.ptx"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/flow_convert.cu"
    COMMENT "Compiling flow_convert.cu to PTX"
    VERBATIM)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/flow_ptx.h"
    COMMAND "${CMAKE_COMMAND}"
            "-DPTX=${CMAKE_CURRENT_BINARY_DIR}/flow_convert.ptx"
            "-DHEADER=${CMAKE_CURRENT_BINARY_DIR}/flow_ptx.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_ptx.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/flow_convert.ptx"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed_ptx.cmake"
    COMMENT "Embedding flow PTX into header"
    VERBATIM)

add_custom_target(flow_ptx_header DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/flow_ptx.h")

# ── Target ──────────────────────────────────────────────────────────────────
add_library(nvof_motion SHARED
    nvof_motion.cpp
)

add_dependencies(nvof_motion flow_ptx_header)

target_include_directories(nvof_motion PRIVATE
    ${RESHADE_INCLUDE}
    ${IMGUI_INCLUDE}
    ${NVOF_SDK}
    ${CUDA_TOOLKIT_ROOT}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(nvof_motion PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ImTextureID=ImU64
)

# ── Output name & extension ─────────────────────────────────────────────────
# ReShade expects  *.addon32  or  *.addon64
set_target_properties(nvof_motion PROPERTIES
    OUTPUT_NAME "nvof_motion"
    PREFIX      ""            # no "lib" prefix on MinGW
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(nvof_motion PROPERTIES SUFFIX ".addon64")
else()
    set_target_properties(nvof_motion PROPERTIES SUFFIX ".addon32")
endif()
