idf_component_register(
  SRCS "factory_main.cpp"
  INCLUDE_DIRS "." "../../include"
  REQUIRES app_update driver esp_partition joltwallet__littlefs mbedtls nvs_flash radar_update
)
