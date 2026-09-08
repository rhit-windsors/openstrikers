cmake_minimum_required(VERSION 3.25)
project(openstrikers CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXTERN_DIR "${CMAKE_SOURCE_DIR}/extern")
set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

add_subdirectory(${EXTERN_DIR}/aurora EXCLUDE_FROM_ALL)

# abseil 20240722.0 gates a WinRT time-zone lookup on NTDDI_WIN10_NI, but the
# WinRT string functions it calls (WindowsCreateStringReference and friends) are
# not declared by mingw-w64's windows.globalization.h chain, so the translation
# unit fails to compile. Request a Windows 10 RTM API level for that one target
# so it takes the portable fallback path instead.
if (MINGW AND TARGET absl_time_zone)
    target_compile_definitions(absl_time_zone PRIVATE NTDDI_VERSION=0x0A000000)
endif ()

file(GLOB_RECURSE NL_SOURCES CONFIGURE_DEPENDS
    ${EXTERN_DIR}/decomp/src/NL/*.cpp
)
# was done automatically by decomp/configure.py calling
# decomp/generate_nlfunction_headers.py
file(COPY_FILE
    "${EXTERN_DIR}/decomp/include/NL/detail/nlFunctionPreProcTemplate.h"
    "${EXTERN_DIR}/decomp/include/NL/detail/nlFunction1PreProcTemplate.h"
    ONLY_IF_DIFFERENT
)

file(GLOB_RECURSE ODE_SOURCES CONFIGURE_DEPENDS
    ${EXTERN_DIR}/decomp/src/ode/*.cpp
    ${EXTERN_DIR}/decomp/src/ode/*.c
)
# An header called "Opcode.h" is in a legal grey area in the original decomp.
# Since the only file that uses it is decomp/src/ode/collision_trimesh_internal.h
# I'm disabling it until I'm in trouble
# For more info, check decomp/THIRD_PARTY_NOTICES.md and
# decomp/docs/ode-provenance.md
list(FILTER ODE_SOURCES EXCLUDE REGEX ".*trimesh.*")
# decomp/src/ode/scrapbook.cpp is obsoleted and shouldn't be compiled
list(FILTER ODE_SOURCES EXCLUDE REGEX ".*scrapbook.*")
# same thing for decomp/src/ode/stack.cpp
list(FILTER ODE_SOURCES EXCLUDE REGEX ".*/ode/stack\\.cpp$")
list(FILTER ODE_SOURCES EXCLUDE REGEX ".*_orig\\.(cpp|c)$")
set_source_files_properties(
    ${ODE_SOURCES}
    PROPERTIES LANGUAGE CXX
)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    ${EXTERN_DIR}/decomp/src/Game/*.cpp
)
list(FILTER GAME_SOURCES EXCLUDE REGEX ".*/AI/Scripts/TestCases\\.cpp$")
# these aren't TUs, their fate is to be included in their base .cpp files
list(FILTER GAME_SOURCES EXCLUDE REGEX ".*_interp\\.cpp$")
list(FILTER GAME_SOURCES EXCLUDE REGEX ".*FloatingPointExceptions\\.cpp$")
list(FILTER GAME_SOURCES EXCLUDE REGEX ".*CallStackDumper\\.cpp$")

add_executable(openstrikers
    ${NL_SOURCES}
    ${ODE_SOURCES}
    ${GAME_SOURCES}
    ${SRC_DIR}/compat_shims.cpp
    ${SRC_DIR}/main.cpp
)

target_include_directories(openstrikers PRIVATE
    ${INCLUDE_DIR}
    ${EXTERN_DIR}/aurora/include
    ${EXTERN_DIR}/decomp/src/ode
    ${EXTERN_DIR}/decomp/include
    ${EXTERN_DIR}/decomp/src
)

target_link_libraries(openstrikers PRIVATE
    aurora::core
    aurora::os
    aurora::gx
    aurora::main
    aurora::mtx
    aurora::pad
    aurora::vi
    aurora::card
    aurora::dvd
)

# Windows links Dawn, SDL3 and nod as DLLs, so they (and the DXC compiler DLLs
# Dawn loads at runtime) have to sit next to the executable. No-op elsewhere.
# The decomp replaces the global operator new/delete (see nlMemory.cpp). With a
# shared libstdc++ those replacements only bind inside this executable, so an
# object allocated here and released inside libstdc++-6.dll would be handed to
# the CRT heap instead of nlFree -- STATUS_HEAP_CORRUPTION. Link the runtime
# statically so there is exactly one operator new in the process.
if (MINGW)
    target_link_options(openstrikers PRIVATE -static-libgcc -static-libstdc++)
endif ()

include(${EXTERN_DIR}/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(openstrikers)

# aurora_copy_runtime_dlls() only knows about imported targets, so the
# toolchain's own DLLs (libwinpthread-1, zlib1, libbz2-1, libsqlite3-0) are left
# behind. They resolve in an MSYS2 shell because ucrt64/bin is already on PATH,
# which hides the problem: run the executable from Explorer or a plain
# PowerShell and the loader fails with 0xC0000135 before main(), printing
# nothing and showing no window. Walk the real import tables and copy whatever
# the toolchain owns, so the build directory is runnable on its own.
if (MINGW)
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH OPENSTRIKERS_TOOLCHAIN_BIN)
    add_custom_command(TARGET openstrikers POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:openstrikers>
            -DOBJDUMP=${CMAKE_OBJDUMP}
            -DBIN_DIRS=${OPENSTRIKERS_TOOLCHAIN_BIN}
            -DSYSTEM_DIR=$ENV{SystemRoot}/System32
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyCompilerRuntimeDLLs.cmake
        VERBATIM
        COMMENT "Copying compiler runtime DLLs next to openstrikers.exe"
    )
endif ()

target_compile_definitions(openstrikers PRIVATE
    MUSY_TARGET=MUSY_TARGET_PC
    TARGET_PC
    dSINGLE=1
    dIDESINGLE
    dNODEBUG=ON
    dTHREADING_INTF_DISABLED
    dODE_SMStricker_Patch
    HAVE_MALLOC_H=1
    MUSY_VERSION_MAJOR=2
    MUSY_VERSION_MINOR=0
    MUSY_VERSION_PATCH=3
    NDEBUG=1
    __IEEE_LITTLE_ENDIAN
    GOLDEN_DISABLE_AUDIO
)

# The allocator's free-list nodes live inside the free memory they describe, so
# a caller overrunning its block is indistinguishable from an allocator bug by
# the time it is visible. This turns on the ring validator in MemAlloc.cpp, which
# checks the invariants on entry to and exit from every Allocate/Free. It is a
# per-source define so toggling it rebuilds one file rather than the whole game.
option(OPENSTRIKERS_MEMALLOC_VALIDATE "Validate the nl free ring on every alloc/free" OFF)
if (OPENSTRIKERS_MEMALLOC_VALIDATE)
    set_source_files_properties(${EXTERN_DIR}/decomp/src/NL/MemAlloc.cpp
        TARGET_DIRECTORY openstrikers
        PROPERTIES COMPILE_DEFINITIONS NL_MEMALLOC_VALIDATE)
endif ()

# A serialized struct's pointer fields are four bytes wide (nlGCPtr32), which is
# lossless only for MEM1 addresses. Storing a host stack or image address in one
# truncates silently and the crash surfaces somewhere else entirely, minutes
# later, depending on where ASLR put the stack. This makes that store abort at
# the point it happens. It is a whole-target define because nlGCPtr32 is a header.
option(OPENSTRIKERS_CHECK_GCPTR "Abort when a non-MEM1 pointer is stored in an nlGCPtr32" OFF)
if (OPENSTRIKERS_CHECK_GCPTR)
    target_compile_definitions(openstrikers PRIVATE NL_GCPTR_CHECKED)
endif ()

# "The screen is black" has too many causes to guess between: nothing submitted,
# everything culled, colour writes masked off, or a scissor that clipped the
# frame away. This reports per-frame draw counts, the logical-vs-render viewport
# and scissor, and each pass's clear value and hasDraws flag, once a second.
# Per-source, so toggling it rebuilds one file rather than the whole game.
option(OPENSTRIKERS_FRAME_STATS "Log per-frame draw counts, viewport/scissor and render passes" OFF)
if (OPENSTRIKERS_FRAME_STATS)
    set_source_files_properties(${EXTERN_DIR}/aurora/lib/gfx/recording.cpp
        TARGET_DIRECTORY aurora_gx
        PROPERTIES COMPILE_DEFINITIONS OPENSTRIKERS_FRAME_STATS)
endif ()

target_compile_options(openstrikers PRIVATE
    -fpermissive
    -include ${INCLUDE_DIR}/compat_shims.h
    -idirafter ${EXTERN_DIR}/decomp/include/PowerPC_EABI_Support/MSL_C/MSL_Common
)
