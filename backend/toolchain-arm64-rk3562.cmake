# RK3562 / AArch64 Linux cross-compilation toolchain.
# Override RK3562_TOOLCHAIN_ROOT in the environment when the SDK is installed elsewhere.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
# Keep try_compile as an executable link check. FindThreads must verify the
# target linker accepts -pthread instead of succeeding on compilation alone.

if(DEFINED ENV{RK3562_TOOLCHAIN_ROOT} AND NOT "$ENV{RK3562_TOOLCHAIN_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{RK3562_TOOLCHAIN_ROOT}" RK3562_TOOLCHAIN_ROOT)
else()
    set(RK3562_TOOLCHAIN_ROOT
        "/home/wu/sdk/kickpi-rk356x/download/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu"
    )
endif()

set(_RK3562_TOOL_PREFIX "${RK3562_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-")

set(CMAKE_C_COMPILER "${_RK3562_TOOL_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_RK3562_TOOL_PREFIX}g++")
set(CMAKE_AR "${_RK3562_TOOL_PREFIX}ar")
set(CMAKE_LINKER "${_RK3562_TOOL_PREFIX}ld")
set(CMAKE_NM "${_RK3562_TOOL_PREFIX}nm")
set(CMAKE_OBJCOPY "${_RK3562_TOOL_PREFIX}objcopy")
set(CMAKE_OBJDUMP "${_RK3562_TOOL_PREFIX}objdump")
set(CMAKE_RANLIB "${_RK3562_TOOL_PREFIX}ranlib")
set(CMAKE_STRIP "${_RK3562_TOOL_PREFIX}strip")

foreach(_tool_var IN ITEMS
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    CMAKE_AR
    CMAKE_LINKER
    CMAKE_NM
    CMAKE_OBJCOPY
    CMAKE_OBJDUMP
    CMAKE_RANLIB
    CMAKE_STRIP
)
    if(NOT EXISTS "${${_tool_var}}")
        message(FATAL_ERROR
            "RK3562 AArch64 toolchain is incomplete: ${_tool_var} was not found at '${${_tool_var}}'. "
            "Set RK3562_TOOLCHAIN_ROOT to the gcc-arm-10.3 AArch64 toolchain root."
        )
    endif()
endforeach()

# The cross compiler carries its target sysroot. Never search target libraries or headers
# from the x86_64 build host through this toolchain root.
set(CMAKE_FIND_ROOT_PATH "${RK3562_TOOLCHAIN_ROOT}/aarch64-none-linux-gnu/libc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_tool_var)
unset(_RK3562_TOOL_PREFIX)
