#pragma once

#include <stdint.h>

#include "tusb.h"

extern const tusb_desc_device_t dln2_device_descriptor;
extern const uint8_t dln2_fs_configuration_descriptor[];
extern const char *dln2_string_descriptor[];
extern const int dln2_string_descriptor_count;
