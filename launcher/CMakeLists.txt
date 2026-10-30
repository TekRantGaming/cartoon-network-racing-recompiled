# The TRG Launcher 2.0 (ext/trg-launcher, launcher2/) for Cartoon Network Racing. Built on its own, apart from the game:
#   cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/launcher
cmake_minimum_required(VERSION 3.20)
project(CnrLauncher C CXX)

set(CNR_LAUNCHER_VERSION 1.0.0)
string(REPLACE "." "," _commas "${CNR_LAUNCHER_VERSION}")

# The launcher runs on its own: no Visual C++ runtime needed beside it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(TRG_LAUNCHER_STANDALONE ON CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_2 ON CACHE BOOL "" FORCE)
set(TRG_LAUNCHER2_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(../ext/trg-launcher trg-launcher)

add_executable(cnr_launcher WIN32 project.cpp launcher.rc)
target_link_libraries(cnr_launcher PRIVATE trg::launcher2)
# PROJECT_VERSION is shown in the launcher; the .rc file uses both.
target_compile_definitions(cnr_launcher PRIVATE PROJECT_VERSION="${CNR_LAUNCHER_VERSION}"
  PROJECT_VERSION_COMMAS=${_commas},0)
if(MSVC)
  target_compile_options(cnr_launcher PRIVATE /W4 /utf-8 /permissive-)
  # A WIN32 program (no console window) that starts at main().
  target_link_options(cnr_launcher PRIVATE /ENTRY:mainCRTStartup)
endif()
