#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "ibbq.h"

#ifdef __cplusplus
extern "C"
{
#endif

    enum SETTINS_ID
    {
        CHANNEL_SETTINGS,
        SYSTEM_SETTINGS,
        GLOBAL_SETTINGS,
    };

    typedef struct system_settings
    {
        char hostname[128];
        char unit[16];
    } system_settings_t;

    typedef struct global_settings
    {
        probe_data_t probe_configs[MAX_PROBE_COUNT];
    } global_settings_t;

    typedef struct channel_settings
    {
        uint8_t version;
        probe_data_t probe_configs[MAX_PROBE_COUNT];
    } channel_settings_t;

    void saveSettings(uint8_t type, void *settings);
    void loadSettings(uint8_t type, void *settings);

#ifdef __cplusplus
}
#endif

#endif