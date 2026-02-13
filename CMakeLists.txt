# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

string(REGEX MATCH "^[^/]+" BOARD_NAME "${BOARD}")
if(BOARD_NAME STREQUAL "rpi_pico2")
    message(STATUS "Compiling for RPI_PICO2")
elseif(BOARD_NAME STREQUAL "rpi_pico")
    message(STATUS "Compiling for RPI_PICO")
endif()

if(EXTRA_CFLAGS MATCHES "-DOVP_SCALE_FACTOR=([0-9\\.eE+-]+)")
  set(OVP_SCALE_FACTOR "${CMAKE_MATCH_1}")
  message(STATUS "OVP_SCALE_FACTOR = ${OVP_SCALE_FACTOR}")
endif()

if(CONFIG_PICO_DISPLAY_PACK AND BOARD_NAME STREQUAL "rpi_pico")
  set(DTC_OVERLAY_FILE "boards/rpi_pico/pico_display_pack.overlay")
elseif(CONFIG_PICO_DISPLAY_PACK2 AND BOARD_NAME STREQUAL "rpi_pico")
  set(DTC_OVERLAY_FILE "boards/rpi_pico/pico_display_pack2.overlay")
elseif(CONFIG_PICO_DISPLAY_PACK AND BOARD_NAME STREQUAL "rpi_pico2")
  set(DTC_OVERLAY_FILE "boards/rpi_pico2/pico_display_pack.overlay")
elseif(CONFIG_PICO_DISPLAY_PACK2 AND BOARD_NAME STREQUAL "rpi_pico2")
  set(DTC_OVERLAY_FILE "boards/rpi_pico2/pico_display_pack2.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zpsu-mon)

add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

include_directories(
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/ui
  )

set(app_SRCS
  ${PROJECT_SOURCE_DIR}/src/led.c
  ${PROJECT_SOURCE_DIR}/src/ui/watchface_ui.c
  ${PROJECT_SOURCE_DIR}/src/ui/screens/ui_Screen1.c
  ${PROJECT_SOURCE_DIR}/src/ui/ui_helpers.c
  ${PROJECT_SOURCE_DIR}/src/ui/ui.c
  ${PROJECT_SOURCE_DIR}/src/ui/fonts/ui_font_DSEG7ClassicBold8.c
  ${PROJECT_SOURCE_DIR}/src/ui/fonts/ui_font_DSEG7ClassicBold36.c
  ${PROJECT_SOURCE_DIR}/src/watchface_app.c
  ${PROJECT_SOURCE_DIR}/src/psu_ctrl.c
  ${PROJECT_SOURCE_DIR}/src/clock.c
  ${PROJECT_SOURCE_DIR}/src/notification_manager.c
  ${PROJECT_SOURCE_DIR}/src/main.c
  ${PROJECT_SOURCE_DIR}/src/display_control.c
  )

target_sources(app PRIVATE ${app_SRCS})

# add_subdirectory(src/ui)
