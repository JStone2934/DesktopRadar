idf_component_register(
  SRCS
    "../../../src/update/update_protocol.cpp"
    "../../../src/storage/update_store.cpp"
  INCLUDE_DIRS "../../../include"
  REQUIRES app_update mbedtls nvs_flash
)
