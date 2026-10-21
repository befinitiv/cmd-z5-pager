target_sources_ifdef(CONFIG_CMDZ5_LCD app PRIVATE display/cmdz5_lcd.c)
target_sources_ifdef(CONFIG_CMDZ5_LCD_BUS_GPIO app PRIVATE display/lcd_bus_gpio.c)
target_sources_ifdef(CONFIG_CMDZ5_AUDIO_PWM app PRIVATE audio/audio_pwm.c)
target_sources_ifdef(CONFIG_CMDZ5_SX1262 app PRIVATE radio/sx1262_mesh.c)
if(CONFIG_CMDZ5_GPS_M10)
  target_sources(app PRIVATE gnss/gps_m10.c)
  # Zephyr's NMEA 0183 helpers have no public header.
  target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/gnss)
endif()
