# Sysbuild entry for the board build (MCUboot + application). The board and
# the devicetree includes and bindings it uses live in the application
# directory, so MCUboot needs them too.
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${APP_DIR})
set(mcuboot_DTS_ROOT ${APP_DIR} CACHE INTERNAL "CMD-Z5 devicetree root for MCUboot")
# An extra overlay: sysbuild/mcuboot.overlay would replace MCUboot's own
# app.overlay, which selects its partition.
set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/sysbuild/mcuboot_usb.overlay
    CACHE INTERNAL "SMP on USB CDC ACM")
# boot/: a screen in USB recovery and leaving it without a host.
set(mcuboot_EXTRA_ZEPHYR_MODULES ${APP_DIR}/boot
    CACHE INTERNAL "CMD-Z5 MCUboot hooks")

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
