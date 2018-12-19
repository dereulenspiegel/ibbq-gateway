#ifndef IWEBSERVER_H
#define IWEBSERVER_H

#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_timer.h"

#include "ibbq.h"

#ifdef __cplusplus
extern "C"
{
#endif

    httpd_handle_t init_webserver(ibbq_state_t *state);
    void stop_webserver(httpd_handle_t server);

    ESP_EVENT_DECLARE_BASE(WIFI_SCAN_EVENT);
    enum
    {
        WIFI_SCAN_REQUESTED
    };
#ifdef __cplusplus
}
#endif
#endif