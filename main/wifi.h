#ifndef WIFI_H
#define WIFI_H

#include "esp_http_server.h"
#include "esp_wifi.h"

#include "ibbq.h"
#include "settings.h"
//#include "wifi_creds.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef WIFI_SSID
#ifndef WIFI_PSK
#error "WiFi PSK not defined, but SSID is defined"
#endif
    static wifi_config_t wifi_config = {
        .sta = {
            {.ssid = WIFI_SSID},
            {.password = WIFI_PSK},
        },
    };
#endif
    typedef struct network_context
    {
        httpd_handle_t webserver;
        ibbq_state_t *bbq_state;
        system_settings_t *sys_settings;
    } network_context_t;

    void wifi_init(network_context_t *ctx);
#ifdef __cplusplus
}
#endif

#endif