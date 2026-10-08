# Zephyr side
target_sources(app PRIVATE
  frontpanel.c
  lcd_bus_emul.c
  lcd_ctrl_model.c
  led_emul.c
  audio_emul.c
  script.c
)

# Host side, built into the native simulator runner against SDL2. The SDL
# compile and link flags come from Zephyr's native_sim SDL support.
target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/frontpanel_bottom.c)
target_compile_options(native_simulator INTERFACE
  -I${CMAKE_CURRENT_SOURCE_DIR}/../lib/gfx/include
  -I${CMAKE_CURRENT_SOURCE_DIR}/../lib/gfx/fonts
)
