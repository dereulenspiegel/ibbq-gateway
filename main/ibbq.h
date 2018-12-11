#ifndef IBBQ_H
#define IBBQ_H

#include "BLEDevice.h"

#define MAX_PROBE_COUNT 8

#define MOCK_IBBQ

#ifdef __cplusplus
extern "C"
{
#endif

    const uint8_t ALARM_LOCAL = 0x01;
    const uint8_t ALARM_CLOUD = 0x02;
    const uint8_t ALARM_IBBQ = 0x04;

    typedef struct probe_data
    {
        float min;
        float max;
        char color[8];
        uint8_t alarm;
        char name[128];
    } probe_data_t;

    typedef struct ibbq_state
    {
        bool connected;
        uint8_t rssi;
        probe_data_t probes[MAX_PROBE_COUNT];
        float temps[MAX_PROBE_COUNT];
        size_t probe_count;
        float battery_percent;
        BLEScan *pBLEScan;
        BLEClient *pClient;
    } ibbq_state_t;

    ibbq_state_t *init_ibbq();

#ifdef __cplusplus
}
#endif
#endif