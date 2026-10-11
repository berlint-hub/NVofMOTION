cmake_minimum_required(VERSION 3.15)
project(GenericDepthEx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths to the reshade-main tree ──────────────────────────────────────────
set(RESHADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../reshade-main" CACHE PATH
    "Root of the crosire/reshade checkout (must contain include/ and deps/)")

set(RESHADE_INCLUDE "${RESHADE_ROOT}/include")
set(IMGUI_INCLUDE   "${RESHADE_ROOT}/deps/imgui")

if(NOT EXISTS "${RESHADE_INCLUDE}/reshade.hpp")
    message(FATAL_ERROR
        "Cannot find reshade.hpp – set RESHADE_ROOT to the reshade-main directory.\n"
        "  Current RESHADE_ROOT = ${RESHADE_ROOT}")
endif()

# ── Target ──────────────────────────────────────────────────────────────────
add_library(generic_depth_ex SHARED
    generic_depth_ex.cpp
)

target_include_directories(generic_depth_ex PRIVATE
    ${RESHADE_INCLUDE}
    ${IMGUI_INCLUDE}
)

target_compile_definitions(generic_depth_ex PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ImTextureID=ImU64
)

# ── Output name & extension ─────────────────────────────────────────────────
# ReShade expects  *.addon32  or  *.addon64
set_target_properties(generic_depth_ex PROPERTIES
    OUTPUT_NAME "generic_depth_ex"
    PREFIX      ""            # no "lib" prefix on MinGW
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(generic_depth_ex PROPERTIES SUFFIX ".addon64")
else()
    set_target_properties(generic_depth_ex PROPERTIES SUFFIX ".addon32")
endif()
