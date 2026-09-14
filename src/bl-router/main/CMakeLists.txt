idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # json for the provisioning payload, nvs_flash for proven credentials,
    # bt for NimBLE, esp_wifi for the join.
    REQUIRES bt nvs_flash esp_wifi esp_netif json
)
