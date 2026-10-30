cmake_minimum_required(VERSION 3.21)
project(CartoonNetworkRacingRecomp CXX C)

# Build layout adapted from KillzoneRecomp (GPL-3.0): PS2Recomp runtime + PCSX2's GS, microVU and SPU2 as libraries.
#
# RelWithDebInfo is the working build. CMake's default for it (/Ob1, security cookies, incremental linking) costs the
# hot generated code measurably. Float semantics stay strict (no /fp:fast): PS2 float emulation depends on it.
option(CNR_FAST_FLAGS "Release-grade optimisation flags for RelWithDebInfo" ON)
# CPU baseline. AVX2 only for now (x86-64-v3); a multi-ISA build like KillzoneRecomp's can come later.
set(KZ_ISA "AVX2" CACHE STRING "CPU baseline for the PCSX2 libraries")
set(KZ_ARCH_FLAG "/arch:AVX2")
set(KZGS_ISA "AVX2" CACHE STRING "" FORCE)
if(MSVC AND CNR_FAST_FLAGS)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- ${KZ_ARCH_FLAG} /DNDEBUG")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- ${KZ_ARCH_FLAG} /DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
endif()

# PS2Recomp runtime + IOP only (the recompiler/analyzer are built separately into build/ps2recomp).
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE) # per-call enter/exit trace
add_subdirectory(ext/PS2Recomp)
add_subdirectory(ext/kzgs)   # PCSX2 hardware GS (ext/kzgs/README.md)
add_subdirectory(ext/kzvu)   # PCSX2 microVU (ext/kzvu/README.md)
add_subdirectory(ext/kzipu)  # PCSX2 IPU, for the game's own movie colour conversion (ext/kzipu/README.md)
add_subdirectory(ext/kzspu2) # PCSX2 SPU2 (ext/kzspu2/README.md)

# Generated code is derived from the game ELF and never committed (scripts/regen.py writes it).
set(CNR_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/usa" CACHE PATH "PS2Recomp output for SLUS_214.38")
file(GLOB CNR_GENERATED_SRC CONFIGURE_DEPENDS "${CNR_GENERATED_DIR}/*.cpp")
list(LENGTH CNR_GENERATED_SRC CNR_GENERATED_COUNT)
message(STATUS "Cartoon Network Racing: ${CNR_GENERATED_COUNT} generated sources")
if(CNR_GENERATED_COUNT EQUAL 0)
    message(FATAL_ERROR "No generated code in ${CNR_GENERATED_DIR}: run `python scripts/regen.py usa` first")
endif()

file(GLOB CNR_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Our host code lives in its own library so its dependencies (SDL3, ...) never change the compile flags of the
# generated-code unity batches (a flag change there means a full rebuild).
find_package(SDL3 CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/ext/sdl3/SDL3-3.4.16/cmake" NO_DEFAULT_PATH)
add_library(cnr_port STATIC ${CNR_SRC})
target_include_directories(cnr_port PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(cnr_port PRIVATE ps2_runtime SDL3::SDL3 kzgs kzvu kzipu kzspu2)
target_compile_definitions(cnr_port PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(cnr ${CNR_GENERATED_SRC})
target_include_directories(cnr PRIVATE "${CNR_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src")
# cnr_port holds main() and the game override registration; link it whole so static registrars are kept.
target_link_libraries(cnr PRIVATE ps2_runtime "$<LINK_LIBRARY:WHOLE_ARCHIVE,cnr_port>" kzgs kzvu kzipu kzspu2)
set_target_properties(cnr PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 128
)
target_precompile_headers(cnr PRIVATE
    ext/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h
    ext/PS2Recomp/ps2xRuntime/include/ps2_runtime.h
    "${CNR_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${CNR_GENERATED_DIR}/ps2_recompiled_stubs.h"
    ext/PS2Recomp/ps2xRuntime/include/ps2_syscalls.h
    ext/PS2Recomp/ps2xRuntime/include/ps2_stubs.h
)
if(MSVC)
    target_compile_options(cnr PRIVATE /bigobj /FS)
    # A window program that starts at main(): no console window when the launcher starts it. Output can still be redirected.
    target_link_options(cnr PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

ps2x_stage_ffmpeg_runtime_dlls(cnr) # FFmpeg DLLs for the runtime's MPEG HLE (FMVs)
kzgs_stage_resources(cnr)           # PCSX2 shaders + runtime DLLs
add_custom_command(TARGET cnr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:cnr>)
