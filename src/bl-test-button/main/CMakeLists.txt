idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # No wifi and no json: this device broadcasts and nothing more.
    REQUIRES bt nvs_flash driver esp_timer
)

# See the router's CMakeLists for why this is needed: idf.py -D sets a CMake
# variable, not a compiler define, and nothing bridges the two.
if(DEFINED NDW_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NDW_VERSION="${NDW_VERSION}")
endif()
