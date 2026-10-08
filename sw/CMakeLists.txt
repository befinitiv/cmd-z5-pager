cmake_minimum_required(VERSION 3.20.0)

# The cmdz5 board definition lives in boards/befi/cmdz5.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmdz5)

zephyr_include_directories(include)

add_subdirectory(lib/gfx)
add_subdirectory(lib/ui)
add_subdirectory(drivers)
if(CONFIG_CMDZ5_EMUL)
  add_subdirectory(emul)
endif()

target_sources(app PRIVATE
  src/main.c
  src/clock.c
  src/config.c
  src/hw.c
  src/i18n.c
  src/input.c
  src/power.c
  src/statusbar.c
  src/screens/home.c
  src/screens/hwtest.c
  src/screens/menu.c
  src/screens/poweroff.c
  src/screens/settings.c
)
