#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_system.h"

#ifdef __cplusplus

extern "C"
{
#endif

    typedef struct dns_server_config
    {
        bool answer_all;
        const char *hostname;
    } dns_server_config_t;

    void init_dns_server(dns_server_config_t *cfg);
#ifdef __cplusplus
}
#endif
#endif
