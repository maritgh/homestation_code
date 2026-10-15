# idf_component_register(
#     SRCS "copy_paste.c"
#     INCLUDE_DIRS "."
#     REQUIRES driver
# )
idf_component_register(
    SRCS "multiple_ir.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_event esp_netif nvs_flash esp_http_server json mbedtls
)