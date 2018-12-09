#ifndef IBBQ_EVENTS_H
#define IBBQ_EVENTS_H

#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(IBBQ_EVENTS);

    enum
    {
        IBBQ_START_SCAN,
        IBBQ_DISCOVERED,
        IBBQ_CONNECTED,
        IBBQ_AUTHENTICATED,
        IBBQ_REQUEST_STATE
    };

#ifdef __cplusplus
}
#endif

#endif