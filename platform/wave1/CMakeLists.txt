if(NOT TARGET jack_wave1_runtime)
    if(NOT DEFINED PICO_SHARED_PATH)
        message(FATAL_ERROR "PICO_SHARED_PATH is required")
    endif()
    if(NOT DEFINED PICO_PIO_USB_PATH)
        message(FATAL_ERROR "PICO_PIO_USB_PATH is required")
    endif()

    set(WAVE1_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT EXISTS "${PICO_SHARED_PATH}/drivers/pico_hdmi/video_output.c")
        message(FATAL_ERROR "pico_shared checkout not found: ${PICO_SHARED_PATH}")
    endif()

    add_subdirectory("${PICO_SHARED_PATH}/drivers/pico_fatfs"
                     "${CMAKE_BINARY_DIR}/pico_fatfs")

    if(JACK_WAVE1_HDMI_AUDIO)
        if(JACK_WAVE1_HDMI_DRIVER_ROOT)
            set(HDMI_DRIVER_ROOT "${JACK_WAVE1_HDMI_DRIVER_ROOT}")
        else()
            set(HDMI_DRIVER_ROOT "${PICO_SHARED_PATH}/drivers/pico_hdmi")
        endif()
        if(NOT JACK_WAVE1_HDMI_ADAPTER)
            set(JACK_WAVE1_HDMI_ADAPTER "${WAVE1_DIR}/hdmi/bytepusher_hdmi.c")
        endif()
        add_library(jack_wave1_hdmi INTERFACE)
        target_sources(jack_wave1_hdmi INTERFACE
            "${HDMI_DRIVER_ROOT}/video_output.c"
            "${HDMI_DRIVER_ROOT}/hstx_packet.c"
            "${JACK_WAVE1_HDMI_ADAPTER}"
            "${WAVE1_DIR}/hdmi/jack_di_queue.c")
        target_include_directories(jack_wave1_hdmi INTERFACE "${HDMI_DRIVER_ROOT}")
        target_compile_definitions(jack_wave1_hdmi INTERFACE NOHSTXCLOCK=1)
        set(WAVE1_VIDEO_TARGET jack_wave1_hdmi)
    else()
        add_subdirectory("${PICO_SHARED_PATH}/drivers/pico_hstx"
                         "${CMAKE_BINARY_DIR}/pico_hstx")
        set(WAVE1_VIDEO_TARGET pico_hstx)
    endif()

    set(SAVED_TINYUSB_TOP "${TOP}")
    set(TOP "${PICO_SDK_PATH}/lib/tinyusb")
    check_and_add_pico_pio_usb_support()
    set(TOP "${SAVED_TINYUSB_TOP}")
    if(NOT TARGET tinyusb_pico_pio_usb)
        message(FATAL_ERROR "Pico-PIO-USB support could not be configured")
    endif()

    add_library(jack_wave1_runtime STATIC
        "${WAVE1_DIR}/jack_wave1_runtime.cpp"
        "${WAVE1_DIR}/jack_wave1_usb_fruit_jam.cpp"
        "${PICO_SHARED_PATH}/nespad.cpp")
    target_include_directories(jack_wave1_runtime PUBLIC
        "${WAVE1_DIR}"
        "${PICO_SHARED_PATH}"
        "${PICO_SHARED_PATH}/drivers/pico_fatfs"
        "${PICO_SHARED_PATH}/drivers/pico_fatfs/fatfs")
    target_compile_definitions(jack_wave1_runtime PUBLIC
        HW_CONFIG=8
        SDCARD_PIN_CS=39
        SDCARD_PIN_SCK=34
        SDCARD_PIN_MOSI=35
        SDCARD_PIN_MISO=36
        SDCARD_PIO=pio0
        SDCARD_SPI=spi0
        GPIOHSTXD0=15
        GPIOHSTXD1=17
        GPIOHSTXD2=19
        GPIOHSTXCK=13
        GPIOHSTXINVERTED=1
        CFG_TUH_RPI_PIO_USB=1
        PIO_USB_USE_PIO=2
        PICO_DEFAULT_PIO_USB_DP_PIN=1)
    target_compile_options(jack_wave1_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    if(JACK_WAVE1_HDMI_AUDIO)
        target_compile_definitions(jack_wave1_runtime PUBLIC JACK_WAVE1_HDMI_AUDIO=1)
    endif()
    target_link_libraries(jack_wave1_runtime PUBLIC
        pico_stdlib
        pico_multicore
        hardware_clocks
        hardware_dma
        hardware_pio
        hardware_spi
        hardware_timer
        hardware_vreg
        pico_fatfs
        ${WAVE1_VIDEO_TARGET}
        tinyusb_host
        tinyusb_pico_pio_usb)
endif()
