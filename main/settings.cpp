#include "settings.h"
#include "ibbq.h"

#include "esp_spiffs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define STORAGE_NAMESPACE "ibbq"

static const char *TAG = "settings";

static system_settings_t *sys_settings;

void writeToFile(const char *key, void *settings, size_t len)
{
    ESP_LOGI(TAG, "Writing %d bytes to key %s", len, key);

    nvs_handle my_handle;
    ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle));
    ESP_ERROR_CHECK(nvs_set_blob(my_handle, key, settings, len));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    nvs_close(my_handle);
}

void readFromFile(const char *key, void *settings, size_t *len)
{
    ESP_LOGI(TAG, "Reading %d bytes from key %s", *len, key);

    nvs_handle my_handle;
    esp_err_t ret;
    ret = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "Reopening NVS with readwrite to create namespace");
        *len = 0;
        nvs_close(my_handle);
        ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle));
        ESP_ERROR_CHECK(nvs_commit(my_handle));
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Returning with 0 bytes read, after creating namespace");
        return;
    }
    else if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Encountered unexpected error when reading from key %s: %s", key, esp_err_to_name(ret));
        nvs_close(my_handle);
        *len = 0;
        return;
    }
    ret = nvs_get_blob(my_handle, key, settings, len);
    if (ret != ESP_OK)
    {
        *len = 0;
        ESP_LOGW(TAG, "Failed to read from NVS key %s: %s", key, esp_err_to_name(ret));
    }
    nvs_close(my_handle);
}

probe_data_t *defaultChannelConfig()
{
    probe_data_t *pb = (probe_data_t *)malloc(sizeof(probe_data_t) * MAX_PROBE_COUNT);
    for (int i = 0; i < MAX_PROBE_COUNT; i++)
    {
        pb[i].min = 0.0f;
        pb[i].max = 0.0f;
        pb[i].alarm = 0;
        sprintf(pb[i].color, "%s", "#22B14C");
        sprintf(pb[i].name, "Kanal %d", i + 1);
    }

    return pb;
}

system_settings_t *defaultSystemSettings()
{
    system_settings_t *settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    sprintf(settings->hostname, "%s", "iBBQ-Gateway");
    sprintf(settings->unit, "%s", "C");
    sprintf(settings->ap_name, "%s", "ibbq-ap");
    sprintf(settings->lang, "%s", "de");

    return settings;
}

void saveSettings(SETTINS_ID type, void *settings)
{
    ESP_LOGD(TAG, "Saving settings: %d", type);
    switch (type)
    {
    case CHANNEL_SETTINGS:
    {
        channel_settings_t cs = {};
        cs.version = 1;
        memcpy(cs.probe_configs, settings, sizeof(cs.probe_configs));
        writeToFile("channels", (uint8_t *)&cs, sizeof(cs));
        break;
    }
    case SYSTEM_SETTINGS:
    {
        system_settings_t *sys_settings = (system_settings_t *)settings;
        sys_settings->version = 1;
        writeToFile("system", settings, sizeof(system_settings_t));
        break;
    }
    default:
    {
        ESP_LOGE(TAG, "Unknown settings value");
        break;
    }
    }
}

void loadSettings(SETTINS_ID type, void *settings)
{
    ESP_LOGI(TAG, "Loading settings: %d", type);
    size_t len = 0;
    switch (type)
    {
    case CHANNEL_SETTINGS:
    {
        channel_settings_t cs = {};
        len = sizeof(cs);
        readFromFile("channels", (uint8_t *)&cs, &len);
        if (len == 0)
        {
            probe_data_t *def_chan_conf = defaultChannelConfig();
            ESP_LOGI(TAG, "Settings never persited yet");
            memcpy(settings, def_chan_conf, sizeof(probe_data_t) * MAX_PROBE_COUNT);
            ESP_LOGI(TAG, "Returning default channel settings");
            free(def_chan_conf);
            return;
        }

        if (cs.version == 1)
        {
            memcpy(settings, cs.probe_configs, sizeof(cs.probe_configs));
        }
        else
        {
            probe_data_t *def_chan_conf = defaultChannelConfig();
            memcpy(settings, def_chan_conf, sizeof(probe_data_t[MAX_PROBE_COUNT]));
            ESP_LOGE(TAG, "Unknown version for channel settings: %d", cs.version);
            free(def_chan_conf);
        }
        break;
    }
    case SYSTEM_SETTINGS:
    {
        len = sizeof(system_settings_t);
        readFromFile("system", (uint8_t *)settings, &len);
        if (len == 0)
        {
            memcpy(settings, defaultSystemSettings(), sizeof(system_settings_t));
            return;
        }

        system_settings_t *sys_settings = (system_settings_t *)settings;
        if (sys_settings->version == 1)
        {
            return;
        }
        else
        {
            memcpy(settings, defaultSystemSettings(), sizeof(system_settings_t));
            ESP_LOGE(TAG, "Unknown system settings version: %d", sys_settings->version);
        }
        break;
    }
    default:
    {
        ESP_LOGE(TAG, "Unknown settings value");
        break;
    }
    }
}
