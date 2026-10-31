cmake_minimum_required(VERSION 3.13)

# Pico SDK location: pass -DPICO_SDK_PATH=... once (cached in the build dir),
# or set the PICO_SDK_PATH environment variable. Built against SDK 2.3.1.
set(PICO_BOARD pico)
include(pico_sdk_import.cmake)

project(stepper_fw C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(stepper_fw
    main.c
)

target_compile_options(stepper_fw PRIVATE -Wall)
target_link_libraries(stepper_fw pico_stdlib)

# .uf2 for drag-and-drop flashing, plus .bin/.hex/.map
pico_add_extra_outputs(stepper_fw)
