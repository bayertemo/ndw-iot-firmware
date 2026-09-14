idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # json for the provisioning payload, nvs_flash for proven credentials,
    # bt for NimBLE, esp_wifi for the join.
    REQUIRES bt nvs_flash esp_wifi esp_netif json
)

# Carry the version from the build into the firmware.
#
# `idf.py -DNDW_VERSION=...` sets a CMake variable, not a compiler define —
# the two are different things and nothing bridges them automatically. Without
# this line every board reported the 0.0.0-dev fallback regardless of what CI
# computed, which is precisely the undetectable lie the -rc scheme exists to
# prevent: a bench board flashed from a branch would have been
# indistinguishable from a release.
if(DEFINED NDW_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NDW_VERSION="${NDW_VERSION}")
endif()
