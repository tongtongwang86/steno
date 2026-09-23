# SPDX-License-Identifier: MIT
#
# Compiled into the `app` target rather than a zephyr_library(), which does
# not inherit ZMK's include directories.
if(CONFIG_STENO_DISPLAY)
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/display/steno_display.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/display)
endif()

if(CONFIG_STENO_DICT)
  target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/sdic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/dict/steno_dict.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dict)
endif()

if(CONFIG_STENO_ZMK)
  target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/steno/steno_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/steno/steno_zmk.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/steno)
endif()
