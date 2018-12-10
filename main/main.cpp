#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi.h"
#include "ibbq.h"
#include "settings.h"

static const char *TAG = "main";

extern "C"
{
    void app_main();
}

void print_reset_reason(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_UNKNOWN:
        ESP_LOGI(TAG, "Reset reason unknown");
        break;
    case ESP_RST_POWERON:
        ESP_LOGI(TAG, "Reset reason power on");
        break;
    case ESP_RST_EXT:
        ESP_LOGI(TAG, "Reset reason external pin (should not happen on ESP32");
        break;
    case ESP_RST_SW:
        ESP_LOGI(TAG, "Reset reason software reset");
        break;
    case ESP_RST_PANIC:
        ESP_LOGI(TAG, "Reset reason panic/exception");
        break;
    case ESP_RST_INT_WDT:
        ESP_LOGI(TAG, "Reset reason interrupt watchdog");
        break;
    case ESP_RST_TASK_WDT:
        ESP_LOGI(TAG, "Reset reason task watchdog");
        break;
    case ESP_RST_WDT:
        ESP_LOGI(TAG, "Reset reason other watchdog");
        break;
    case ESP_RST_DEEPSLEEP:
        ESP_LOGI(TAG, "Reset reason wakeup from deep sleep");
        break;
    case ESP_RST_BROWNOUT:
        ESP_LOGI(TAG, "Reset reason brownout");
        break;
    case ESP_RST_SDIO:
        ESP_LOGI(TAG, "Reset readon SDIO");
        break;
    }
}

void app_main()
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    print_reset_reason(reset_reason);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    printf("Hello world!\n");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    ibbq_state_t *bbq_state = init_ibbq();
    loadSettings(CHANNEL_SETTINGS, bbq_state->probes);
    static network_context_t nCtx;
    nCtx.bbq_state = bbq_state;
    //ibbq_state_t bbq_state = {};
    //nCtx.bbq_state = &bbq_state;
    wifi_init_sta(&nCtx);
}