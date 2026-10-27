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

add_library(iw2_video
  src/video/roq.cpp)
target_include_directories(iw2_video PUBLIC src)
target_compile_options(iw2_video PRIVATE -Wall -Wextra -Wpedantic)

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
  src/game/combat.cpp src/game/bulletdamage.cpp src/game/playerweapon.cpp src/game/sounds.cpp src/game/menus.cpp src/game/hud.cpp src/game/inventory.cpp src/game/movers.cpp src/game/animscripted.cpp src/game/playerlink.cpp src/game/rockets.cpp src/game/viewweapon.cpp src/game/playerevents.cpp src/game/shellshock.cpp src/game/shellshockworld.cpp src/game/itempickup.cpp src/game/itempickupworld.cpp src/game/triggerdamage.cpp
  src/game/vehicles.cpp src/game/vehicledrive.cpp src/game/campaign.cpp src/game/fx.cpp src/game/marks.cpp src/game/perception.cpp
  src/game/turrets.cpp src/game/turretthink.cpp src/game/entityclips.cpp src/game/actorturret.cpp src/game/hitboxes.cpp
  src/game/actorstate.cpp src/game/actorcover.cpp src/game/actorslide.cpp
  src/game/actorstate.cpp src/game/actorcover.cpp src/game/friendlychain.cpp src/game/friendlychainworld.cpp
  src/game/actorpath.cpp src/game/actorfollow.cpp src/game/badplace.cpp src/game/grenades.cpp src/game/actorgrenade.cpp src/game/actorevents.cpp
  src/game/watersim.cpp src/game/clientinput.cpp src/game/playeractions.cpp src/game/playeruse.cpp)
target_include_directories(iw2_game PUBLIC src)
target_link_libraries(iw2_game PUBLIC iw2_fs iw2_script iw2_assets)
target_compile_options(iw2_game PRIVATE -Wall -Wextra -Wpedantic)

add_library(iw2_ui
  src/ui/menu.cpp
  src/ui/menurun.cpp
  src/ui/frontend.cpp
  src/ui/profile.cpp
  src/ui/savegame.cpp)
target_include_directories(iw2_ui PUBLIC src)
target_link_libraries(iw2_ui PUBLIC iw2_fs)
target_compile_options(iw2_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(iw2game tools/iw2game/main.cpp)
target_link_libraries(iw2game PRIVATE iw2_game iw2_audio)

add_executable(iw2fs tools/iw2fs/main.cpp)
target_link_libraries(iw2fs PRIVATE iw2_fs)

add_executable(iw2asset tools/iw2asset/main.cpp)
target_link_libraries(iw2asset PRIVATE iw2_fs iw2_assets iw2_audio)

add_executable(iw2roq tools/iw2roq/main.cpp)
target_link_libraries(iw2roq PRIVATE iw2_fs iw2_assets iw2_video)

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
  target_link_libraries(iw2view PRIVATE iw2_game iw2_audio iw2_ui iw2_video SDL3::SDL3)
  target_compile_options(iw2view PRIVATE -Wall -Wextra)
else()
  message(STATUS "SDL3 not found: iw2view will not be built (brew install sdl3)")
endif()

add_executable(test_screen_blur tests/test_screen_blur.cpp)
target_link_libraries(test_screen_blur PRIVATE iw2_game)
add_test(NAME screen_blur COMMAND test_screen_blur)

add_executable(test_xmodel tests/test_xmodel.cpp)
target_link_libraries(test_xmodel PRIVATE iw2_assets)
add_test(NAME xmodel COMMAND test_xmodel)

add_executable(test_xanim tests/test_xanim.cpp)
target_link_libraries(test_xanim PRIVATE iw2_assets)
add_test(NAME xanim COMMAND test_xanim)

add_executable(test_lightgrid tests/test_lightgrid.cpp)
target_link_libraries(test_lightgrid PRIVATE iw2_assets)
add_test(NAME lightgrid COMMAND test_lightgrid)

add_executable(test_vm tests/test_vm.cpp)
target_link_libraries(test_vm PRIVATE iw2_script)
add_test(NAME vm COMMAND test_vm)

add_executable(test_playerweapon tests/test_playerweapon.cpp)
target_link_libraries(test_playerweapon PRIVATE iw2_game)
add_test(NAME playerweapon COMMAND test_playerweapon)
add_executable(test_bulletdamage tests/test_bulletdamage.cpp)
target_link_libraries(test_bulletdamage PRIVATE iw2_game)
add_test(NAME bulletdamage COMMAND test_bulletdamage)
add_executable(test_idledefer tests/test_idledefer.cpp)
target_link_libraries(test_idledefer PRIVATE iw2_game)
add_test(NAME idledefer COMMAND test_idledefer)

add_executable(test_clientinput tests/test_clientinput.cpp)
target_link_libraries(test_clientinput PRIVATE iw2_game)
add_test(NAME clientinput COMMAND test_clientinput)

add_executable(test_recoil tests/test_recoil.cpp)
target_link_libraries(test_recoil PRIVATE iw2_game)
add_test(NAME recoil COMMAND test_recoil)
add_executable(test_viewimpulses tests/test_viewimpulses.cpp)
target_link_libraries(test_viewimpulses PRIVATE iw2_game)
add_test(NAME viewimpulses COMMAND test_viewimpulses)
add_executable(test_playerevents tests/test_playerevents.cpp)
target_link_libraries(test_playerevents PRIVATE iw2_game)
add_test(NAME playerevents COMMAND test_playerevents)
add_executable(test_actorslide tests/test_actorslide.cpp)
target_link_libraries(test_actorslide PRIVATE iw2_game)
add_test(NAME actorslide COMMAND test_actorslide)

add_executable(test_shellshock tests/test_shellshock.cpp)
target_link_libraries(test_shellshock PRIVATE iw2_game)
add_test(NAME shellshock COMMAND test_shellshock)
add_executable(test_friendlychain tests/test_friendlychain.cpp)
target_link_libraries(test_friendlychain PRIVATE iw2_game)
add_test(NAME friendlychain COMMAND test_friendlychain)

add_executable(test_itempickup tests/test_itempickup.cpp)
target_link_libraries(test_itempickup PRIVATE iw2_game)
add_test(NAME itempickup COMMAND test_itempickup)

add_executable(test_sound tests/test_sound.cpp)
target_link_libraries(test_sound PRIVATE iw2_audio)
add_test(NAME sound COMMAND test_sound)

add_executable(test_menu tests/test_menu.cpp)
target_link_libraries(test_menu PRIVATE iw2_ui)
add_test(NAME menu COMMAND test_menu)
add_executable(test_menurun tests/test_menurun.cpp)
target_link_libraries(test_menurun PRIVATE iw2_ui)
add_test(NAME menurun COMMAND test_menurun)

add_executable(test_movers tests/test_movers.cpp)
target_link_libraries(test_movers PRIVATE iw2_game)
add_test(NAME movers COMMAND test_movers)

add_executable(test_animtree tests/test_animtree.cpp)
target_link_libraries(test_animtree PRIVATE iw2_game)
add_test(NAME animtree COMMAND test_animtree)

add_executable(test_efx tests/test_efx.cpp)
target_link_libraries(test_efx PRIVATE iw2_game)
add_test(NAME efx COMMAND test_efx)

add_executable(test_hitboxes tests/test_hitboxes.cpp)
target_link_libraries(test_hitboxes PRIVATE iw2_game)
add_test(NAME hitboxes COMMAND test_hitboxes)

add_executable(test_pmove tests/test_pmove.cpp)
target_link_libraries(test_pmove PRIVATE iw2_game)
add_test(NAME pmove COMMAND test_pmove)

add_executable(test_vehicledrive tests/test_vehicledrive.cpp)
target_link_libraries(test_vehicledrive PRIVATE iw2_game)
add_test(NAME vehicledrive COMMAND test_vehicledrive)

add_executable(test_texscroll tests/test_texscroll.cpp)
target_link_libraries(test_texscroll PRIVATE iw2_game)
add_test(NAME texscroll COMMAND test_texscroll)

add_executable(test_watersim tests/test_watersim.cpp)
target_link_libraries(test_watersim PRIVATE iw2_game)
add_test(NAME watersim COMMAND test_watersim)

add_executable(test_fog tests/test_fog.cpp)
target_link_libraries(test_fog PRIVATE iw2_game)
add_test(NAME fog COMMAND test_fog)

add_executable(test_frontend tests/test_frontend.cpp)
target_link_libraries(test_frontend PRIVATE iw2_ui)
add_test(NAME frontend COMMAND test_frontend)

add_executable(test_profile tests/test_profile.cpp)
target_link_libraries(test_profile PRIVATE iw2_ui)
add_test(NAME profile COMMAND test_profile)

add_executable(test_savegame tests/test_savegame.cpp)
target_link_libraries(test_savegame PRIVATE iw2_ui)
add_test(NAME savegame COMMAND test_savegame)

add_executable(test_compassspring tests/test_compassspring.cpp)
target_link_libraries(test_compassspring PRIVATE iw2_game)
add_test(NAME compassspring COMMAND test_compassspring)
add_executable(test_screenfade tests/test_screenfade.cpp)
target_link_libraries(test_screenfade PRIVATE iw2_game)
add_test(NAME screenfade COMMAND test_screenfade)

add_executable(test_roq tests/test_roq.cpp)
target_link_libraries(test_roq PRIVATE iw2_video)
add_test(NAME roq COMMAND test_roq)

add_executable(test_triggerdamage tests/test_triggerdamage.cpp)
target_link_libraries(test_triggerdamage PRIVATE iw2_game)
add_test(NAME triggerdamage COMMAND test_triggerdamage)
