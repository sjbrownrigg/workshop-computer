cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(stepbridge C CXX ASM)

pico_sdk_init()

add_executable(stepbridge
        main.cpp
        sequencer.cpp
        usb_link.cpp
        usb_descriptors.c
        flash_store.cpp
        )

# copy_to_ram: per the Workshop Computer's own AI directive and the prior
# project's flash-safety notes, this is tried first (before any
# __not_in_flash_func/multicore_lockout work) since it puts the whole
# binary in RAM and plausibly avoids that class of bug entirely.
pico_set_binary_type(stepbridge copy_to_ram)

# Without this, firmware works once after flashing but fails after a reset.
target_compile_definitions(stepbridge PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(stepbridge PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(stepbridge
        pico_stdlib
        pico_multicore
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_adc
        hardware_dma
        hardware_i2c
        hardware_pwm
        hardware_spi
        hardware_flash
        )

pico_enable_stdio_usb(stepbridge 0)
pico_add_extra_outputs(stepbridge)
