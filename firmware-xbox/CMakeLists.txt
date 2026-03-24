cmake_minimum_required(VERSION 3.13)

# ============================================================================
# firmware-xbox: RP2040 #1 — Xbox GIP Device + Equation VM + SPI Master
# ============================================================================

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(firmware_xbox C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Main executable
add_executable(firmware_xbox
    src/main.c
    src/gip_protocol.c
    src/gip_wheel.c
    src/gip_metadata.c
    src/eq_vm.c
    src/eq_opcodes.c
    src/spi_master.c
    src/diagnostics.c
    src/usb_descriptors.c
)

target_include_directories(firmware_xbox PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(firmware_xbox
    pico_stdlib
    pico_multicore
    pico_float          # ROM-accelerated float routines
    hardware_spi
    hardware_watchdog
    hardware_timer
    tinyusb_device
)

# UART for debug (native USB IS the Xbox device — cannot use for stdio)
pico_enable_stdio_usb(firmware_xbox 0)
pico_enable_stdio_uart(firmware_xbox 1)

# Generate UF2
pico_add_extra_outputs(firmware_xbox)

# Compiler warnings
target_compile_options(firmware_xbox PRIVATE -Wall -Wextra -Wno-unused-parameter)
