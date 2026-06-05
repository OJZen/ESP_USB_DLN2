idf_component_register(
    SRCS "dln2.c" "dln2_gpio.c" "dln2_i2c.c" "dln2_pin.c" "dln2_spi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_timer
)
