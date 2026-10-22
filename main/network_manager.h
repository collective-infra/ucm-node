#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "esp_err.h"

typedef enum {
    NETWORK_NONE,
    NETWORK_ETHERNET,
    NETWORK_WIFI
} network_type_t;

esp_err_t network_start(void);

#endif