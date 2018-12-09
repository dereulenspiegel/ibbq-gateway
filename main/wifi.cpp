#include "wifi.h"

#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mdns.h"

#include "webserver.h"
#include "wifi_creds.h"

#define CONFIG_ESP_MAXIMUM_RETRY 50

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static esp_err_t esp_wifi_event_handler(void *ctx, system_event_t *event)
{
    network_context_t *nCtx = (network_context_t *)ctx;
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        //tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "Got IPv4: %s", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        s_retry_num = 0;
        nCtx->webserver = init_webserver(nCtx->bbq_state);
        break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
        ip6_addr_t ip_info;
        ESP_ERROR_CHECK(tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &ip_info));
        //ESP_LOGI(TAG, "IPv6 address received: %s", IPV62STR(ip_info));
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    {
        ESP_LOGW(TAG, "Disconnected from WiFi");
        if (nCtx->webserver)
        {
            ESP_LOGI(TAG, "Stopping webserver");
            stop_webserver(nCtx->webserver);
            nCtx->webserver = NULL;
        }
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            xEventGroupClearBits(s_wifi_event_group, CONFIG_ESP_MAXIMUM_RETRY);
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG, "connect to the AP fail\n");
        break;
    }
    default:
        break;
    }
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}

void wifi_init_sta(network_context_t *ctx)
{
    s_wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(esp_wifi_event_handler, ctx));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            {.ssid = WIFI_SSID},
            {.password = WIFI_PSK}},
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s", WIFI_SSID);
}