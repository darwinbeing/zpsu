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

# Conditionally layer the PIO+DMA display fragment on top of the base overlay.
# This must be done from CMake (not via an #if inside the overlay): Zephyr runs
# the devicetree through the C preprocessor before Kconfig is evaluated, so
# CONFIG_* macros are not defined during DT preprocessing. The pio dtsi is its
# own overlay so it is applied after the base one and repoints &mipi_dbi.
# The PIO fragment is panel-agnostic (the display SPI pins are identical on
# Pack 1.0 and Pack 2.0), so a single per-board-family fragment is layered on
# top of whichever pack overlay was selected above.
if(CONFIG_DISPLAY_SPI_PIO_DMA)
  if(BOARD_NAME STREQUAL "rpi_pico")
    list(APPEND DTC_OVERLAY_FILE "boards/rpi_pico/pico_display_pio.dtsi")
  elseif(BOARD_NAME STREQUAL "rpi_pico2")
    list(APPEND DTC_OVERLAY_FILE "boards/rpi_pico2/pico_display_pio.dtsi")
  endif()
endif()

# Pico W WiFi/AP builds (wifi.conf / ap.conf) need a flash storage partition for
# NVS-backed Settings. DT is preprocessed before Kconfig, so gate on the conf
# fragment (a CMake-time -D), not CONFIG_WIFI. These builds are always the
# rpi_pico/rp2040/w board, where &flash0 / &code_partition exist.
if("${EXTRA_CONF_FILE}" MATCHES "wifi.conf" OR "${EXTRA_CONF_FILE}" MATCHES "ap.conf")
  list(APPEND DTC_OVERLAY_FILE "boards/pico_w_storage.overlay")
endif()

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zpsu-mon)

add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

if(OVP_SCALE_FACTOR)
  add_compile_definitions(OVP_SCALE_FACTOR=${OVP_SCALE_FACTOR})
endif()

include_directories(
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/ui
  ${PROJECT_SOURCE_DIR}/src/psu
  ${PROJECT_SOURCE_DIR}/src/app
  ${PROJECT_SOURCE_DIR}/src/clock
  ${PROJECT_SOURCE_DIR}/src/notification
  ${PROJECT_SOURCE_DIR}/src/display
  ${PROJECT_SOURCE_DIR}/src/driver
  ${PROJECT_SOURCE_DIR}/src/led
  ${PROJECT_SOURCE_DIR}/src/common
  )

set(app_SRCS
  ${PROJECT_SOURCE_DIR}/src/led/led.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/watchface.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/splash.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/screens/ui_Screen1.c
  ${PROJECT_SOURCE_DIR}/src/ui/ui_helpers.c
  ${PROJECT_SOURCE_DIR}/src/ui/ui.c
  ${PROJECT_SOURCE_DIR}/src/ui/fonts/ui_font_DSEG7ClassicBold8.c
  ${PROJECT_SOURCE_DIR}/src/ui/fonts/ui_font_DSEG7ClassicBold36.c
  ${PROJECT_SOURCE_DIR}/src/app/watchface_app.cpp
  ${PROJECT_SOURCE_DIR}/src/app/watchface_app_glue.c
  ${PROJECT_SOURCE_DIR}/src/psu/psu_service.cpp
  ${PROJECT_SOURCE_DIR}/src/psu/psu_channel.c
  ${PROJECT_SOURCE_DIR}/src/psu/psu_controller.cpp
  ${PROJECT_SOURCE_DIR}/src/clock/clock.cpp
  ${PROJECT_SOURCE_DIR}/src/notification/notification_manager.cpp
  ${PROJECT_SOURCE_DIR}/src/main.cpp
  ${PROJECT_SOURCE_DIR}/src/app/app.cpp
  ${PROJECT_SOURCE_DIR}/src/display/display_control.cpp
  )

target_sources(app PRIVATE ${app_SRCS})

if(CONFIG_DISPLAY_SPI_PIO_DMA)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/driver/spi_pio_dma.c)
  # spi_context.h is a private header that lives alongside the in-tree SPI
  # drivers; make it resolvable from our out-of-tree driver.
  target_include_directories(app PRIVATE $ENV{ZEPHYR_BASE}/drivers/spi)
endif()

# Thin app-side trigger module for the opt-in MCUboot USB-DFU upgrade build.
# Only compiled when MCUboot is the bootloader (dfu.conf / sysbuild sets this);
# it references bootmode_set / sys_reboot. Gated on the Kconfig symbol (set
# after find_package), NOT on BOARD — a sysbuild child image does not see BOARD
# reliably before find_package. The shell `dfu` command inside dfu.c is further
# gated on CONFIG_SHELL (present on the WiFi/AP builds). Default builds, which
# have no MCUboot, never compile this.
if(CONFIG_BOOTLOADER_MCUBOOT)
  target_sources(app PRIVATE ${PROJECT_SOURCE_DIR}/src/dfu/dfu.c)
  target_include_directories(app PRIVATE ${PROJECT_SOURCE_DIR}/src/dfu)
endif()

# Pico W WiFi connectivity glue — compiled only for the opt-in STA WiFi build
# (CONFIG_WIFI comes from wifi.conf via -DEXTRA_CONF_FILE). Keeps default
# rpi_pico / rpi_pico2 builds untouched.
# Excluded from the AP build (CONFIG_APP_WIFI_AP=y): the AP glue lives in
# src/net/wifi_ap.c (Task 2); wifi_net.c is the STA-mode glue only.
if(CONFIG_WIFI AND NOT CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_net.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c)
endif()

if(CONFIG_APP_WIFI_AP)
  target_sources(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net/wifi_ap.c
    ${PROJECT_SOURCE_DIR}/src/net/psu_udp.c
    ${PROJECT_SOURCE_DIR}/src/net/persist.c
    ${PROJECT_SOURCE_DIR}/src/net/ap_config.c
    ${PROJECT_SOURCE_DIR}/src/net/cred_parse.c
    # Host-testable DFU verb matcher; psu_udp.c's DFU branch (gated on
    # CONFIG_BOOTLOADER_MCUBOOT) calls it. Harmless on the plain AP build.
    ${PROJECT_SOURCE_DIR}/src/dfu/dfu_cmd.c)
  target_include_directories(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/net
    ${PROJECT_SOURCE_DIR}/src/psu
    ${PROJECT_SOURCE_DIR}/src/dfu)
endif()

# add_subdirectory(src/ui)
