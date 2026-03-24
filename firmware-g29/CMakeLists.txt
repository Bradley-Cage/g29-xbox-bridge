cmake_minimum_required(VERSION 3.13)

# ============================================================================
# firmware-g29: RP2040 #2 — G29 USB Host + Auth Relay + SPI Slave
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
    src/auth_relay.c
    src/spi_slave.c
    src/axis_math.c
    src/diagnostics.c
    # PIO USB HCD for TinyUSB integration
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

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
    pico_pio_usb
)

# UART for debug (native USB is NOT used — PIO handles USB host)
pico_enable_stdio_usb(firmware_g29 0)
pico_enable_stdio_uart(firmware_g29 1)

# Generate UF2
pico_add_extra_outputs(firmware_g29)

# Compiler warnings + print memory usage
target_link_options(firmware_g29 PRIVATE -Xlinker --print-memory-usage)
target_compile_options(firmware_g29 PRIVATE -Wall -Wextra -Wno-unused-parameter)
