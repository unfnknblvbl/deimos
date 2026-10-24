# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# Deimos Editor (Master System, Game Gear, Mega Drive, SNES, 3DS, 32X, Game Boy Color, Game Boy Advance)
# Use ./build.sh (see README.md): it puts each platform's editor in build/<platform>/.
#
# Targets:
#   editor_core        a static library: everything without a window (ROMs,
#                      levels, builders, renderers; the engines are files beside it)
#   deimos-editor-cli  the command-line tool (src/main_cli_only.cpp)
#   deimos-editor      the editor ("Deimos Editor"), Dear ImGui on SDL2
cmake_minimum_required(VERSION 3.16)
project(deimos_editor C CXX)

# the version (VERSION): 0.<the consoles it makes ROMs for>.<the releases since the last console came in>
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION EDITOR_VERSION)
string(STRIP "${EDITOR_VERSION}" EDITOR_VERSION)
add_compile_definitions(EDITOR_VERSION="${EDITOR_VERSION}")   # the version as a C string
# re-run CMake when VERSION changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(EDITOR_FETCH_SDL "Download and build SDL2 instead of using the system's" OFF)
# (Windows builds always build SDL2 themselves: a static library in the .exe)
if(WIN32)
  set(EDITOR_FETCH_SDL ON)
endif()

# ---- SDL2 ----
if(NOT EDITOR_FETCH_SDL)
  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found on the system: it will be downloaded and built")
    set(EDITOR_FETCH_SDL ON)
  endif()
endif()
# SDL2 from its git repository, as a static library only (no tests, no install)
if(EDITOR_FETCH_SDL)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                       GIT_TAG release-2.32.10 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(SDL_LIBS SDL2::SDL2main SDL2::SDL2-static)
else()
  # the system's SDL2: shared if it has one, else static; plus SDL2main if present
  if(TARGET SDL2::SDL2)
    set(SDL_LIBS SDL2::SDL2)
  else()
    set(SDL_LIBS SDL2::SDL2-static)
  endif()
  if(TARGET SDL2::SDL2main)
    list(PREPEND SDL_LIBS SDL2::SDL2main)
  endif()
endif()

# ---- the consoles' engines ----
# Not built in: the editor reads them from the engines/ folder beside it
# (src/engine.cpp), which build.sh copies from engines/*.eng.
set(ENGINE_SRC)

# ---- Deimos's own title art (art/*.png, from megadrive/tools/make_title_art.py) ----
# Each picture becomes art_<name>[] and art_<name>_size (read by src/assets.cpp).
foreach(pic title_bg title_logo title_icon)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/art_${pic}.cpp)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/art/${pic}.png -DOUT=${out}
            -DNAME=art_${pic} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/art/${pic}.png ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding the title art ${pic}")
  list(APPEND ENGINE_SRC ${out})
endforeach()

# ---- the editor's core (no GUI): project, level, renderer, builders ----
add_library(editor_core STATIC ${ENGINE_SRC} src/engine.cpp
  src/zx0.cpp src/project.cpp src/projectfile.cpp src/colour.cpp src/geometry.cpp src/levelblob.cpp src/keytext.cpp
  src/sms_build.cpp src/sms_model.cpp src/md_build.cpp src/rom.cpp src/n3ds.cpp src/mars.cpp src/gbc.cpp src/gba.cpp src/vb.cpp src/projectio.cpp src/edit.cpp src/cli.cpp
  src/render.cpp src/console_view.cpp src/jar.cpp src/assets.cpp src/md_areas.c)
# the colour choices must come out the same on every machine (no fused multiply-add)
target_compile_options(editor_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
target_include_directories(editor_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(editor_core PUBLIC Threads::Threads)

# command-line only tool (no window): selftest / rebuild / export / import
add_executable(deimos-editor-cli src/main_cli_only.cpp)
target_link_libraries(deimos-editor-cli PRIVATE editor_core)

# ---- the editor ----
set(IMGUI third_party/imgui)
add_executable(deimos-editor WIN32
  src/main.cpp src/app.cpp src/ui_map.cpp src/ui_art.cpp src/ui_data.cpp src/scripts.cpp
  src/settings.cpp src/filedialog.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/imgui_demo.cpp ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(deimos-editor PRIVATE ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(deimos-editor PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(deimos-editor PRIVATE editor_core ${SDL_LIBS})
set_target_properties(deimos-editor PROPERTIES OUTPUT_NAME "Deimos Editor")

if(WIN32)
  # one self-contained .exe: no MinGW DLLs to carry around
  target_link_options(deimos-editor PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(deimos-editor-cli PRIVATE -static -static-libgcc -static-libstdc++)
else()
  # the C++ runtime inside the program: it runs on Linux machines with another compiler's
  target_link_options(deimos-editor PRIVATE -static-libgcc -static-libstdc++)
  target_link_options(deimos-editor-cli PRIVATE -static-libgcc -static-libstdc++)
endif()
