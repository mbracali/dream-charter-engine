# gd-tool-02: Dream Charter Engine app. C++17 + SDL3, like gd-engine-launcher.
# The Python helpers (python/: metadata, Demucs + ai_charter forge, export) run as child processes.
cmake_minimum_required(VERSION 3.24)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GDT02_VERSION LIMIT_COUNT 1)
project(gd_tool_02 VERSION ${GDT02_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)
add_executable(gd-tool-02 src/main.cpp src/app.cpp src/board.cpp src/chart.cpp src/screen_home.cpp src/screen_forge.cpp
  src/screen_tune.cpp src/screen_export.cpp src/selftest.cpp src/stems.cpp src/worker.cpp src/yaml_doc.cpp)
target_link_libraries(gd-tool-02 PRIVATE SDL3::SDL3)
# Where the tool finds .venv/, python/ and models/.
target_compile_definitions(gd-tool-02 PRIVATE GDT02_ROOT="${CMAKE_CURRENT_SOURCE_DIR}" GDT02_VERSION="${GDT02_VERSION}")
if(MSVC)
  target_compile_options(gd-tool-02 PRIVATE /W4 /permissive-)
else()
  target_compile_options(gd-tool-02 PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Tests: ctest --test-dir build
enable_testing()
add_executable(worker_test tests/worker_test.cpp src/worker.cpp)
target_link_libraries(worker_test PRIVATE SDL3::SDL3)
add_test(NAME worker_delivers_every_line COMMAND worker_test)
add_executable(chart_test tests/chart_test.cpp src/chart.cpp)
target_link_libraries(chart_test PRIVATE SDL3::SDL3)
add_test(NAME chart_round_trip_and_grid COMMAND chart_test)
