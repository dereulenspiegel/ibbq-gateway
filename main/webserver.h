#ifndef IWEBSERVER_H
#define IWEBSERVER_H

#include "esp_http_server.h"

#include "ibbq.h"

#ifdef __cplusplus
extern "C"
{
#endif

    httpd_handle_t init_webserver(ibbq_state_t *state);
    void stop_webserver(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
#endif