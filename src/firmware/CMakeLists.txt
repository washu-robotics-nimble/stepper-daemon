cmake_minimum_required(VERSION 3.13)

# Pico SDK location: pass -DPICO_SDK_PATH=... once (cached in the build dir),
# or set the PICO_SDK_PATH environment variable. Built against SDK 2.3.1.
set(PICO_BOARD pico)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # build/compile_commands.json for the IDE
include(pico_sdk_import.cmake)

project(stepper_fw C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../lib)

add_executable(stepper_fw
    main.c
    board.c
    board_table.c
    hostlink.c
    linebuf.c
    usb_descriptors.c
    ${LIB_DIR}/cJSON/cJSON.c
)

target_compile_options(stepper_fw PRIVATE -Wall)

# tusb_config.h lives next to the sources
target_include_directories(stepper_fw PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${LIB_DIR}/cJSON
)

target_link_libraries(stepper_fw
    pico_stdlib
    pico_unique_id
    hardware_gpio
    tinyusb_device
    tinyusb_board
)

# The host link owns USB, and UART0's pins stay free
pico_enable_stdio_usb(stepper_fw 0)
pico_enable_stdio_uart(stepper_fw 0)

# .uf2 for drag-and-drop flashing, plus .bin/.hex/.map
pico_add_extra_outputs(stepper_fw)
