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
        WIFI_SETTINGS,
    };

    typedef struct system_settings
    {
        uint8_t version;
        char hostname[128];
        char unit[2];
        char lang[3];
        char ap_name[32];
    } system_settings_t;

    /*typedef struct global_settings
    {
        probe_data_t probe_configs[MAX_PROBE_COUNT];
    } global_settings_t;*/

    typedef struct wifi_client_config
    {
        uint8_t version;
        char ssid[32];
        char psk[64];
    } wifi_client_config_t;

    typedef struct channel_settings
    {
        uint8_t version;
        probe_data_t probe_configs[MAX_PROBE_COUNT];
    } channel_settings_t;

    void saveSettings(SETTINS_ID type, void *settings);
    bool loadSettings(SETTINS_ID type, void *settings);

#ifdef __cplusplus
}
#endif

#endif