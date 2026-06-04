#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLN2_MAX_PACKET_SIZE 512

void dln2_init(void);
size_t dln2_handle_request(const uint8_t *request, size_t request_len,
                           uint8_t *response, size_t response_cap);

#ifdef __cplusplus
}
#endif
