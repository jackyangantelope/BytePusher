cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Adafruit Fruit Jam" FORCE)
set(PICO_PSRAM_CS_PIN 47 CACHE STRING "Fruit Jam PSRAM chip select" FORCE)
set(PICO_PSRAM_SIZE_BYTES 0x800000 CACHE STRING "Fruit Jam PSRAM size" FORCE)
set(PICO_DEFAULT_UART 0 CACHE STRING "Diagnostic UART" FORCE)
set(PICO_DEFAULT_UART_TX_PIN 44 CACHE STRING "Diagnostic UART TX" FORCE)
set(PICO_DEFAULT_UART_RX_PIN 45 CACHE STRING "Diagnostic UART RX" FORCE)
set(PICO_FLASH_SPI_CLKDIV 10 CACHE STRING "Conservative 480 MHz XIP boot divider" FORCE)

if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to Pico SDK 2.3.0 or newer")
endif()
set(PICO_SHARED_PATH "${CMAKE_CURRENT_LIST_DIR}/deps/pico_shared" CACHE PATH "pico_shared source")
if(NOT DEFINED PICO_PIO_USB_PATH)
    message(FATAL_ERROR "Set PICO_PIO_USB_PATH to a Pico-PIO-USB checkout")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(picoBytePusher C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_library(bp_pager STATIC "${CMAKE_CURRENT_LIST_DIR}/core/bp_pager.c")
target_include_directories(bp_pager PUBLIC "${CMAKE_CURRENT_LIST_DIR}/core")

set(JACK_WAVE1_HDMI_AUDIO ON)
set(JACK_WAVE1_HDMI_DRIVER_ROOT "${CMAKE_CURRENT_LIST_DIR}/deps/bytepusher_hdmi")
set(JACK_WAVE1_HDMI_ADAPTER "${CMAKE_CURRENT_LIST_DIR}/firmware/bytepusher_hdmi.c")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/platform/wave1" "${CMAKE_BINARY_DIR}/wave1")
target_compile_definitions(jack_wave1_runtime PRIVATE
    JACK_WAVE1_SYS_CLOCK_KHZ=480000u
    JACK_WAVE1_FLASH_MAX_HZ=40000000u
    JACK_WAVE1_PSRAM_MAX_HZ=96000000u
    JACK_WAVE1_EXTERNAL_HSTX_CLOCK=1)
target_link_libraries(jack_wave1_runtime PRIVATE hardware_pll hardware_psram)

option(BUILD_FOR_BOOTLOADER "Link to the resident loader app partition" ON)
add_executable(picoBytePusher
    "${CMAKE_CURRENT_LIST_DIR}/firmware/bytepusher_main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/firmware/bytepusher_log15.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/firmware/bytepusher_clock13.cpp")
pico_set_program_name(picoBytePusher "picoBytePusher")
pico_set_program_version(picoBytePusher "0.10.5-fruit-jam-candidate")
pico_enable_stdio_uart(picoBytePusher 1)
pico_enable_stdio_usb(picoBytePusher 0)
target_include_directories(picoBytePusher PRIVATE "${CMAKE_CURRENT_LIST_DIR}/firmware")
target_compile_definitions(picoBytePusher PRIVATE
    HW_CONFIG=8
    BYTEPUSHER_PROFILE_OC480=1
    PICO_STACK_SIZE=4096
    PICO_PSRAM_CS_PIN=47
    PICO_PSRAM_SIZE_BYTES=0x800000
    JACK_WAVE1_CONTINUOUS_SILENCE=1
    JACK_BYTEPUSHER_HSTX_DIAG_R12=1
    JACK_BYTEPUSHER_QUIET_CORE1_R15=1)
target_compile_options(picoBytePusher PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(picoBytePusher PRIVATE
    pico_stdlib hardware_psram hardware_pll bp_pager jack_wave1_runtime)
target_link_options(picoBytePusher PRIVATE -Wl,--print-memory-usage)
if(BUILD_FOR_BOOTLOADER)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/LoaderPartition.cmake")
    bytepusher_link_for_loader(picoBytePusher)
endif()
pico_add_extra_outputs(picoBytePusher)
