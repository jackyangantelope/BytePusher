function(bytepusher_link_for_loader TARGET)
    set(LOADER_APP_BASE 0x10080000)
    set(LOADER_APP_SIZE 0xF80000)
    set(FRUIT_JAM_FLASH_SIZE 0x1000000)

    # Override the SDK flash region while retaining its RAM and stack layout.
    set(LOADER_LINKER_DIR "${CMAKE_CURRENT_BINARY_DIR}/loader_linker")
    file(MAKE_DIRECTORY "${LOADER_LINKER_DIR}")
    file(WRITE "${LOADER_LINKER_DIR}/pico_flash_region.ld"
        "FLASH(rx) : ORIGIN = ${LOADER_APP_BASE}, LENGTH = ${LOADER_APP_SIZE}\n")
    pico_add_linker_script_override_path(${TARGET} "${LOADER_LINKER_DIR}")

    target_compile_definitions(${TARGET} PRIVATE
        PICO_FLASH_SIZE_BYTES=${FRUIT_JAM_FLASH_SIZE})
    set_target_properties(${TARGET} PROPERTIES
        PICOTOOL_EXTRA_UF2_ARGS "--platform;rp2350")
    message(STATUS "Loader partition: ${TARGET} -> ${LOADER_APP_BASE} (${LOADER_APP_SIZE} bytes)")
endfunction()
