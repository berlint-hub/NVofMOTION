cmake_minimum_required(VERSION 3.15)
project(NvofMotion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths ───────────────────────────────────────────────────────────────────
set(RESHADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../reshade-main" CACHE PATH
    "Root of the crosire/reshade checkout (must contain include/ and deps/)")
# Root of the NvOF headers (nvOpticalFlowCommon.h and
# nvOpticalFlowD3D12.h). Points at NvOFInterface/ of the mbucchia/Optical-Flow-SDK
# mirror (API 5.0; NVIDIA's own GitHub is stale at 2.0).
set(NVOF_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../nvof-sdk" CACHE PATH
    "Root of the NvOFInterface headers directory")

set(RESHADE_INCLUDE "${RESHADE_ROOT}/include")
set(IMGUI_INCLUDE   "${RESHADE_ROOT}/deps/imgui")

if(NOT EXISTS "${RESHADE_INCLUDE}/reshade.hpp")
    message(FATAL_ERROR
        "Cannot find reshade.hpp – set RESHADE_ROOT to the reshade-main directory.\n"
        "  Current RESHADE_ROOT = ${RESHADE_ROOT}")
endif()
if(NOT EXISTS "${NVOF_SDK}/nvOpticalFlowCommon.h")
    message(FATAL_ERROR
        "Cannot find nvOpticalFlowCommon.h – set NVOF_SDK to the NvOFInterface directory.\n"
        "  Current NVOF_SDK = ${NVOF_SDK}")
endif()

# ── Target (pure C++, no CUDA: the D3D12 backend binds nvofapi64.dll
# explicitly at runtime and links nothing) ───────────────────────────────────
add_library(nvof_motion SHARED
    nvof_motion.cpp
)

target_include_directories(nvof_motion PRIVATE
    ${RESHADE_INCLUDE}
    ${IMGUI_INCLUDE}
    ${NVOF_SDK}
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
