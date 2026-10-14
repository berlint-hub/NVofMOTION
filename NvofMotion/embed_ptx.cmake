# embed_ptx.cmake - embed PTX bytes as a C array (xxd-free).
# Usage: cmake -DPTX=<in.ptx> -DHEADER=<out.h> -P embed_ptx.cmake
if(NOT PTX OR NOT HEADER)
    message(FATAL_ERROR "embed_ptx.cmake needs -DPTX and -DHEADER")
endif()
file(READ "${PTX}" HEXDATA HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1, " BYTEDATA "${HEXDATA}")
file(WRITE "${HEADER}"
    "#pragma once\n"
    "// Generated from flow_convert.ptx - do not edit.\n"
    "static const unsigned char FLOW_PTX_BYTES[] = {\n${BYTEDATA}\n};\n"
    "static const size_t FLOW_PTX_SIZE = sizeof(FLOW_PTX_BYTES);\n")
