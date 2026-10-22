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
  src/assets/xmodel.cpp
  src/assets/lightgrid.cpp
  src/assets/xanim.cpp
  src/assets/font.cpp
  src/assets/efx.cpp
  src/util/png.cpp)
target_include_directories(iw2_assets PUBLIC src)
target_link_libraries(iw2_assets PUBLIC ZLIB::ZLIB)
target_compile_options(iw2_assets PRIVATE -Wall -Wextra -Wpedantic)

add_library(iw2_audio
  src/audio/sound.cpp
  src/audio/decode.cpp)
target_include_directories(iw2_audio PUBLIC src)
target_link_libraries(iw2_audio PUBLIC iw2_fs)
if(APPLE)
  # MP3 music is decoded with the system AudioToolbox framework (src/audio/decode_apple.cpp).
  target_sources(iw2_audio PRIVATE src/audio/decode_apple.cpp)
  target_link_libraries(iw2_audio PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
else()
  message(FATAL_ERROR "mp3 decoding is only implemented with AudioToolbox (macOS)")
endif()
target_compile_options(iw2_audio PRIVATE -Wall -Wextra -Wpedantic)

add_library(iw2_script
  src/script/parser.cpp
  src/script/value.cpp
  src/script/vm.cpp
  src/script/builtins_core.cpp)
target_include_directories(iw2_script PUBLIC src)
target_compile_options(iw2_script PRIVATE -Wall -Wextra -Wpedantic)

add_executable(iw2script tools/iw2script/main.cpp)
target_link_libraries(iw2script PRIVATE iw2_fs iw2_script iw2_assets)

add_library(iw2_game
  src/game/world.cpp
  src/game/collision.cpp
  src/game/pmove.cpp
  src/game/animtree.cpp
  src/game/pathnodes.cpp
  src/game/actorfields.cpp
  src/game/combat.cpp src/game/playerweapon.cpp src/game/sounds.cpp src/game/hud.cpp src/game/inventory.cpp src/game/movers.cpp src/game/viewweapon.cpp
  src/game/vehicles.cpp src/game/campaign.cpp)
target_include_directories(iw2_game PUBLIC src)
target_link_libraries(iw2_game PUBLIC iw2_fs iw2_script iw2_assets)
target_compile_options(iw2_game PRIVATE -Wall -Wextra -Wpedantic)

add_library(iw2_ui
  src/ui/menu.cpp)
target_include_directories(iw2_ui PUBLIC src)
target_link_libraries(iw2_ui PUBLIC iw2_fs)
target_compile_options(iw2_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(iw2game tools/iw2game/main.cpp)
target_link_libraries(iw2game PRIVATE iw2_game iw2_audio)

add_executable(iw2fs tools/iw2fs/main.cpp)
target_link_libraries(iw2fs PRIVATE iw2_fs)

add_executable(iw2asset tools/iw2asset/main.cpp)
target_link_libraries(iw2asset PRIVATE iw2_fs iw2_assets iw2_audio)

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
  target_link_libraries(iw2view PRIVATE iw2_game iw2_audio SDL3::SDL3)
  target_compile_options(iw2view PRIVATE -Wall -Wextra)
else()
  message(STATUS "SDL3 not found: iw2view will not be built (brew install sdl3)")
endif()

add_executable(test_xmodel tests/test_xmodel.cpp)
target_link_libraries(test_xmodel PRIVATE iw2_assets)
add_test(NAME xmodel COMMAND test_xmodel)

add_executable(test_lightgrid tests/test_lightgrid.cpp)
target_link_libraries(test_lightgrid PRIVATE iw2_assets)
add_test(NAME lightgrid COMMAND test_lightgrid)

add_executable(test_vm tests/test_vm.cpp)
target_link_libraries(test_vm PRIVATE iw2_script)
add_test(NAME vm COMMAND test_vm)

add_executable(test_playerweapon tests/test_playerweapon.cpp)
target_link_libraries(test_playerweapon PRIVATE iw2_game)
add_test(NAME playerweapon COMMAND test_playerweapon)

add_executable(test_sound tests/test_sound.cpp)
target_link_libraries(test_sound PRIVATE iw2_audio)
add_test(NAME sound COMMAND test_sound)

add_executable(test_menu tests/test_menu.cpp)
target_link_libraries(test_menu PRIVATE iw2_ui)
add_test(NAME menu COMMAND test_menu)

add_executable(test_movers tests/test_movers.cpp)
target_link_libraries(test_movers PRIVATE iw2_game)
add_test(NAME movers COMMAND test_movers)

add_executable(test_efx tests/test_efx.cpp)
target_link_libraries(test_efx PRIVATE iw2_assets)
add_test(NAME efx COMMAND test_efx)
