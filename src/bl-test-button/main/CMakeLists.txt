idf_component_register(
    SRCS "main.c" "ndw_console.c"
    INCLUDE_DIRS "."
    # No wifi: this device broadcasts and nothing more. json is for the console
    # the cable speaks, and mbedtls for the CCM that authenticates a beacon.
    REQUIRES bt nvs_flash driver esp_timer json mbedtls esp_driver_usb_serial_jtag
)

# See the router's CMakeLists for why this is needed: idf.py -D sets a CMake
# variable, not a compiler define, and nothing bridges the two.
if(DEFINED NDW_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NDW_VERSION="${NDW_VERSION}")
endif()
