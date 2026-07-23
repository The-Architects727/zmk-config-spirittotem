if(CONFIG_TOTEM_BATTERY_LED)
  zephyr_library()
  # ZMK's own public headers (zmk/event_manager.h, zmk/events/*.h) live under
  # zmk/app/include, which is a PRIVATE include dir on ZMK's "app" target and
  # isn't inherited by out-of-tree zephyr_library() modules automatically.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/battery_led.c)
endif()
