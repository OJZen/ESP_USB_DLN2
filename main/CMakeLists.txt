idf_component_register(
    SRCS "main.c" "usb_descriptors.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES dln2 espressif__esp_tinyusb espressif__tinyusb
)
