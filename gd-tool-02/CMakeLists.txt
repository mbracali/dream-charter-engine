# gd-tool-02: Stem Viewer (spike). C++17 + SDL3, like gd-engine-launcher.
# The Python helpers (python/: Demucs separation, metadata), run as a child process.
cmake_minimum_required(VERSION 3.24)
project(gd_tool_02 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)
add_executable(gd-tool-02 src/main.cpp src/app.cpp src/forge.cpp src/stems.cpp src/worker.cpp src/yaml_doc.cpp)
target_link_libraries(gd-tool-02 PRIVATE SDL3::SDL3)
# Where the tool finds .venv/, python/ and models/.
target_compile_definitions(gd-tool-02 PRIVATE GDT02_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
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
