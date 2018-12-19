#ifndef WIFI_H
#define WIFI_H

#include "esp_http_server.h"
#include "ibbq.h"
#include "settings.h"

#ifdef __cplusplus
extern "C"
{
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