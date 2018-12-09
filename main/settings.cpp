#include "settings.h"
#include "ibbq.h"

#include "esp_spiffs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define STORAGE_NAMESPACE "ibbq"

static const char *TAG = "settings";

void writeToFile(const char *key, void *settings, size_t len)
{
    ESP_LOGI(TAG, "Writing %d bytes to key %s", len, key);

    nvs_handle my_handle;
    ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle));
    ESP_ERROR_CHECK(nvs_set_blob(my_handle, key, settings, len));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    nvs_close(my_handle);
}

void readFromFile(const char *key, void *settings, size_t len)
{
    ESP_LOGI(TAG, "Reading %d bytes from key %s", len, key);

    nvs_handle my_handle;
    ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle));
    ESP_ERROR_CHECK(nvs_get_blob(my_handle, key, settings, &len));
    nvs_close(my_handle);
}

void saveSettings(uint8_t type, void *settings)
{
    ESP_LOGI(TAG, "Saving settings: %d", type);
    switch (type)
    {
    case CHANNEL_SETTINGS:
        channel_settings_t cs = {};
        cs.version = 1;
        memcpy(cs.probe_configs, settings, sizeof(cs.probe_configs));
        writeToFile("channels", (uint8_t *)&cs, sizeof(cs));
        break;
    }
}

void loadSettings(uint8_t type, void *settings)
{
    ESP_LOGI(TAG, "Loading settings: %d", type);
    switch (type)
    {
    case CHANNEL_SETTINGS:
        channel_settings_t cs = {};
        readFromFile("channels", (uint8_t *)&cs, sizeof(cs));
        if (cs.version == 1)
        {
            memcpy(settings, cs.probe_configs, sizeof(cs.probe_configs));
        }
        else
        {
            ESP_LOGE(TAG, "Unknown version for channel settings: %d", cs.version);
        }
    }
}
