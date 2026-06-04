idf_component_register(
    SRCS "dln2.c" "dln2_gpio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio
)
