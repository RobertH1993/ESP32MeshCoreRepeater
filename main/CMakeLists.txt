idf_component_register(SRCS "main.c" "sx126x_hal.c" "lora.c" "display.c"
                            "sx126x_driver/src/sx126x.c" "heltecv4.c" "meshcore/meshcore_packet.c" "meshcore/mesh.c"
                            "ed25519/keypair.c" "ed25519/sign.c" "ed25519/verify.c" "ed25519/add_scalar.c"
                            "ed25519/key_exchange.c" "ed25519/fe.c" "ed25519/ge.c" "ed25519/sc.c" "ed25519/sha512.c"
                       PRIV_REQUIRES spi_flash esp_lcd esp_driver_i2c esp_driver_gpio nvs_flash esp_driver_spi mbedtls
                       INCLUDE_DIRS "." "ed25519")
