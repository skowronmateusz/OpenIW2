cmake_minimum_required(VERSION 3.25)

# Target Apple Silicon even when CMake itself runs under Rosetta (an x86_64 cmake reports an x86_64 host).
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture")
endif()
project(openiw2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(ZLIB REQUIRED)

add_library(iw2_fs
  src/fs/zip_archive.cpp
  src/fs/vfs.cpp)
target_include_directories(iw2_fs PUBLIC src)
target_link_libraries(iw2_fs PUBLIC ZLIB::ZLIB)
target_compile_options(iw2_fs PRIVATE -Wall -Wextra -Wpedantic)

add_library(iw2_assets
  src/assets/iwi.cpp
  src/assets/d3dbsp.cpp
  src/assets/material.cpp
  src/util/png.cpp)
target_include_directories(iw2_assets PUBLIC src)
target_link_libraries(iw2_assets PUBLIC ZLIB::ZLIB)
target_compile_options(iw2_assets PRIVATE -Wall -Wextra -Wpedantic)

add_executable(iw2fs tools/iw2fs/main.cpp)
target_link_libraries(iw2fs PRIVATE iw2_fs)

add_executable(iw2asset tools/iw2asset/main.cpp)
target_link_libraries(iw2asset PRIVATE iw2_fs iw2_assets)

enable_testing()
add_executable(test_vfs tests/test_vfs.cpp)
target_link_libraries(test_vfs PRIVATE iw2_fs)
add_test(NAME vfs COMMAND test_vfs)

add_executable(test_iwi tests/test_iwi.cpp)
target_link_libraries(test_iwi PRIVATE iw2_assets)
add_test(NAME iwi COMMAND test_iwi)

add_executable(test_d3dbsp tests/test_d3dbsp.cpp)
target_link_libraries(test_d3dbsp PRIVATE iw2_assets)
add_test(NAME d3dbsp COMMAND test_d3dbsp)

find_package(SDL3 CONFIG)
if(SDL3_FOUND)
  add_executable(iw2view apps/iw2view/main.cpp)
  target_link_libraries(iw2view PRIVATE iw2_fs iw2_assets SDL3::SDL3)
  target_compile_options(iw2view PRIVATE -Wall -Wextra)
else()
  message(STATUS "SDL3 not found: iw2view will not be built (brew install sdl3)")
endif()
