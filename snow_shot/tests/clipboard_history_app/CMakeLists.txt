# SPDX-License-Identifier: GPL-3.0-or-later
#
# Standalone, distributable build of the Win+V clipboard-history feature.
#
# This reuses the exact same controller/store translation units that are wired
# into snow_shot (ApplicationController). Those files only depend on Qt
# Core/Gui/Widgets and Win32, so they can be shipped as a small portable
# executable without the full snow_shot toolchain (VS 18 2026, static Qt,
# vcpkg, Rust, OCR).
#
# Build (Windows, official Qt 6.12.0 win64_msvc2022_64):
#   cmake -S snow_shot/tests/clipboard_history_app -B build/clipboard-app `
#         -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt prefix>
#   cmake --build build/clipboard-app --config Release
#
# Distribution: run windeployqt on the resulting exe and zip the folder.

cmake_minimum_required(VERSION 3.24)
project(snow_shot_clipboard_history_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SNOW_SHOT_DIR "${REPO_ROOT}/snow_shot")

add_library(clipboard_history_core STATIC
    "${SNOW_SHOT_DIR}/src/presentation/services/clipboardhistorystore.cpp"
    "${SNOW_SHOT_DIR}/src/presentation/services/clipboardhistorycontroller.cpp")
target_include_directories(clipboard_history_core PUBLIC "${SNOW_SHOT_DIR}/include")
target_link_libraries(clipboard_history_core PUBLIC Qt6::Widgets)
if(WIN32)
    # AddClipboardFormatListener, SetWindowsHookExW, SendInput, ...
    target_link_libraries(clipboard_history_core PUBLIC user32)
endif()
if(MSVC)
    # Chinese UI strings in main.cpp are UTF-8 without a BOM.
    target_compile_options(clipboard_history_core PRIVATE /utf-8)
endif()

add_executable(snow_shot_clipboard_history WIN32 main.cpp)
target_link_libraries(snow_shot_clipboard_history PRIVATE
    clipboard_history_core Qt6::Widgets)
if(TARGET Qt6::EntryPoint)
    target_link_libraries(snow_shot_clipboard_history PRIVATE Qt6::EntryPoint)
endif()
if(MSVC)
    target_compile_options(snow_shot_clipboard_history PRIVATE /utf-8)
endif()
