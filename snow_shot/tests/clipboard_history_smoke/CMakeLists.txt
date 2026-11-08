# SPDX-License-Identifier: GPL-3.0-or-later
#
# Standalone compile/link smoke test for the clipboard-history module.
#
# Deliberately NOT registered in snow_shot/CMakeLists.txt. The full project
# needs a custom static Qt 6.12 kit, Visual Studio 18 2026 and a pinned vcpkg
# baseline, none of which exists on a stock GitHub-hosted runner. The three
# clipboard-history files only depend on Qt Core/Gui/Widgets and Win32, so
# this mini project can compile them against an official Qt desktop kit and
# still catch real C++/Qt/Win32 compile and link errors.
#
# Usage (Windows, after installing Qt 6.12.0 win64_msvc2022_64):
#   cmake -S snow_shot/tests/clipboard_history_smoke -B build/clipboard-smoke `
#         -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt prefix>
#   cmake --build build/clipboard-smoke --config Release
#   ctest --test-dir build/clipboard-smoke -C Release --output-on-failure

cmake_minimum_required(VERSION 3.24)
project(snow_shot_clipboard_history_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SNOW_SHOT_DIR "${REPO_ROOT}/snow_shot")

add_library(snow_shot_clipboard_history STATIC
    "${SNOW_SHOT_DIR}/src/presentation/services/clipboardhistorystore.cpp"
    "${SNOW_SHOT_DIR}/src/presentation/services/clipboardhistorycontroller.cpp")
target_include_directories(snow_shot_clipboard_history PUBLIC "${SNOW_SHOT_DIR}/include")
target_link_libraries(snow_shot_clipboard_history PUBLIC Qt6::Widgets)
if(WIN32)
    # AddClipboardFormatListener, SetWindowsHookExW, SendInput, ...
    target_link_libraries(snow_shot_clipboard_history PUBLIC user32)
endif()

add_executable(clipboard_history_guard_tests
    "${SNOW_SHOT_DIR}/tests/clipboard_history_guard_tests.cpp")
target_include_directories(clipboard_history_guard_tests PRIVATE "${SNOW_SHOT_DIR}/include")
target_link_libraries(clipboard_history_guard_tests PRIVATE Qt6::Core)

# Forces the linker to pull in the controller and store translation units.
add_executable(clipboard_history_smoke main.cpp)
target_link_libraries(clipboard_history_smoke PRIVATE
    snow_shot_clipboard_history Qt6::Widgets)

enable_testing()
add_test(NAME clipboard-history-guard COMMAND clipboard_history_guard_tests)
