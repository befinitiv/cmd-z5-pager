cmake_minimum_required(VERSION 3.20.0)

# The cmdz5 board definition lives in boards/befi/cmdz5.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
# Language model image for the flash (merged.hex), see modules/lm_image.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/lm_image)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmdz5)

zephyr_include_directories(include)

# Git hash and version (git describe) for the About screen, refreshed on every
# build: app_generated/git_version.h.
set(git_version_dir ${CMAKE_CURRENT_BINARY_DIR}/app_generated)
set(git_version_h ${git_version_dir}/git_version.h)
add_custom_target(git_version
  COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${git_version_h}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_version.cmake
  BYPRODUCTS ${git_version_h}
  COMMENT "Reading the git version")
add_dependencies(app git_version)
target_include_directories(app PRIVATE ${git_version_dir})

add_subdirectory(lib/gfx)
add_subdirectory(lib/ui)
add_subdirectory(lib/textinput)
add_subdirectory(lib/meshtastic)
add_subdirectory(lib/synth)
add_subdirectory(lib/qrcodegen)
add_subdirectory(drivers)
if(CONFIG_CMDZ5_EMUL)
  add_subdirectory(emul)
else()
  target_sources(app PRIVATE src/battery_npm13xx.c)
endif()

target_sources(app PRIVATE
  src/main.c
  src/battery.c
  src/clock.c
  src/config.c
  src/factory.c
  src/gauge.c
  src/gps.c
  src/hw.c
  src/i18n.c
  src/input.c
  src/power.c
  src/statusbar.c
  src/textin.c
  src/update.c
  src/mesh/mesh.c
  src/mesh/msglog.c
  src/mesh/msgstore.c
  src/screens/about.c
  src/screens/chat.c
  src/screens/compose.c
  src/screens/diag.c
  src/screens/game.c
  src/screens/witch.c
  src/screens/channels.c
  src/screens/gps.c
  src/screens/home.c
  src/screens/hwtest.c
  src/screens/inbox.c
  src/screens/menu.c
  src/screens/nodes.c
  src/screens/poweroff.c
  src/screens/settings.c
)

# Language models for predictive text, built by tools/lmbuild. Boards with an
# lm partition get them through modules/lm_image (merged.hex); the emulator
# links them into the program.
dt_nodelabel(lm_node NODELABEL lm_partition)
if(NOT lm_node)
  set(lm_gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/lm)
  foreach(lang de en)
    generate_inc_file_for_target(app ${CMAKE_CURRENT_SOURCE_DIR}/lm/${lang}.lm
      ${lm_gen_dir}/${lang}.lm.inc)
  endforeach()
endif()
