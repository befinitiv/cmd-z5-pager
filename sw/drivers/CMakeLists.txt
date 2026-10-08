target_sources_ifdef(CONFIG_CMDZ5_LCD app PRIVATE display/cmdz5_lcd.c)
target_sources_ifdef(CONFIG_CMDZ5_LCD_BUS_GPIO app PRIVATE display/lcd_bus_gpio.c)
target_sources_ifdef(CONFIG_CMDZ5_AUDIO_PWM app PRIVATE audio/audio_pwm.c)
