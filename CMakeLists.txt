if(CONFIG_TOTEM_BATTERY_LED)
  zephyr_library()
  zephyr_library_sources(src/battery_led.c)
endif()
