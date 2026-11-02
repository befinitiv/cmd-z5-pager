# Built into MCUboot only (CONFIG_MCUBOOT_ACTION_HOOKS in sysbuild/mcuboot.conf).
if(CONFIG_MCUBOOT_ACTION_HOOKS)
  zephyr_library()
  zephyr_library_sources(src/boot_hooks.c)
  zephyr_library_include_directories(${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include)
endif()
