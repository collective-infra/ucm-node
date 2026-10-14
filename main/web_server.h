#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

typedef enum {
    WEB_SERVER_MODE_NORMAL,   // dashboard (/, /data) + /update
    WEB_SERVER_MODE_SETUP,    // config form (/setup, /save) + /update; / redirects to /setup
} web_server_mode_t;

httpd_handle_t web_server_start(web_server_mode_t mode);

#endif