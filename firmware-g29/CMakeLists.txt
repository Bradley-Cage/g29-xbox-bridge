cmake_minimum_required(VERSION 3.13)

# ============================================================================
# firmware-g29: RP2040 #2 — G29 USB Host + SPI Slave (auth moved to Board X)
# ============================================================================

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(firmware_g29 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Pico-PIO-USB
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB)
add_subdirectory(${PICO_PIO_USB_PATH} pico_pio_usb)

# TinyUSB PIO-USB HCD (must be explicitly included)
set(PICO_TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)

# Main executable
add_executable(firmware_g29
    src/main.c
    src/g29_host.c
    src/g29_parser.c
    src/g29_ffb.c
    src/spi_slave.c
    src/axis_math.c
    src/diagnostics.c
    src/cdc_debug.c
    src/led_status.c
    # PIO USB HCD for TinyUSB integration
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# WS2812 PIO program for onboard status LED (generates ws2812.pio.h)
pico_generate_pio_header(firmware_g29 ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(firmware_g29 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${PICO_PIO_USB_PATH}/src
)

# PIO_USB_USE_TINYUSB required for HCD integration
target_compile_definitions(firmware_g29 PRIVATE PIO_USB_USE_TINYUSB)

target_link_libraries(firmware_g29
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_spi
    hardware_watchdog
    tinyusb_host
    tinyusb_device
    pico_pio_usb
)

# Debug output: custom CDC device on native USB (NOT pico_stdio_usb — incompatible with tinyusb_host)
# Our cdc_debug module provides printf redirection via TinyUSB device CDC
pico_enable_stdio_usb(firmware_g29 0)
pico_enable_stdio_uart(firmware_g29 0)

# Generate UF2
pico_add_extra_outputs(firmware_g29)

# Compiler warnings + print memory usage
target_link_options(firmware_g29 PRIVATE -Xlinker --print-memory-usage)
target_compile_options(firmware_g29 PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ─── CDC Test Build (minimal, no PIO USB host) ───
add_executable(cdc_test
    src/cdc_test.c
)
target_link_libraries(cdc_test pico_stdlib)
pico_enable_stdio_usb(cdc_test 1)
pico_enable_stdio_uart(cdc_test 0)
pico_add_extra_outputs(cdc_test)
