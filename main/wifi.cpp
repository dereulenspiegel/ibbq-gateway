#include "wifi.h"

#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mdns.h"
#include "esp_timer.h"

#include "webserver.h"
//#include "wifi_creds.h"
#include "settings.h"
#include "dns_server.h"

#define CONFIG_ESP_MAXIMUM_RETRY 50
#define MAX_STA_CONN 2
#define DEFAULT_WIFI_PASS "ibbq-wifi"

static const char *TAG = "wifi station";

static dns_server_config_t dns_config = {
    .answer_all = false,
    .hostname = "ibbq.gateway."};

static void connect_timeout_timer_callback(void *arg);
static esp_timer_handle_t connect_timeout_timer;
esp_timer_create_args_t connect_timeout_timer_args;

//static EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static esp_err_t esp_wifi_event_handler(void *ctx, system_event_t *event)
{
    network_context_t *nCtx = (network_context_t *)ctx;
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "Setting hostname to %s", nCtx->sys_settings->hostname);
        ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, nCtx->sys_settings->hostname));
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Connected to AP");
        //tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "Got IPv4: %s", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        s_retry_num = 0;
        nCtx->webserver = init_webserver(nCtx->bbq_state);
        esp_timer_stop(connect_timeout_timer);
        break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
        //ip6_addr_t ip_info;
        //ESP_ERROR_CHECK(tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &ip_info));
        //ESP_LOGI(TAG, "IPv6 address received: %s", IPV62STR(ip_info));
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    {
        ESP_LOGW(TAG, "Disconnected from WiFi");
        if (nCtx->webserver)
        {
            ESP_LOGD(TAG, "Stopping webserver");
            stop_webserver(nCtx->webserver);
            nCtx->webserver = NULL;
        }

        ESP_LOGI(TAG, "Retrying connection");
        esp_wifi_connect();
        //xEventGroupClearBits(s_wifi_event_group, CONFIG_ESP_MAXIMUM_RETRY);
        s_retry_num++;
        ESP_LOGD(TAG, "retry to connect to the AP");
        break;
    }
    case SYSTEM_EVENT_AP_STACONNECTED:
    {
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    }
    case SYSTEM_EVENT_AP_STADISCONNECTED:
    {
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    }
    default:
        break;
    }
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}

void wifi_init_ap(network_context_t *ctx)
{
    ESP_LOGI(TAG, "WiFi unconfigured, starting AP...");
    system_settings_t *sys_settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    loadSettings(SYSTEM_SETTINGS, sys_settings);

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.password, DEFAULT_WIFI_PASS, 10);
    wifi_config.ap.max_connection = MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    strncpy((char *)wifi_config.ap.ssid, sys_settings->ap_name, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    free(sys_settings);
    init_dns_server(&dns_config);
    ctx->webserver = init_webserver(ctx->bbq_state);
}

static void connect_timeout_timer_callback(void *arg)
{
    ESP_LOGW(TAG, "Seems we failed to properly connect to the WiFi");
    ESP_ERROR_CHECK(esp_wifi_stop());

    network_context_t *ctx = (network_context_t *)arg;
    wifi_init_ap(ctx);
}

void wifi_init(network_context_t *ctx)
{
    //s_wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(esp_wifi_event_handler, ctx));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_client_config_t *client_config = (wifi_client_config_t *)malloc(sizeof(wifi_client_config_t));
    if (!loadSettings(WIFI_SETTINGS, client_config))
    {
        wifi_init_ap(ctx);
    }
    else
    {
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, client_config->ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, client_config->psk, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "wifi_init_sta finished.");
        ESP_LOGI(TAG, "connect to ap SSID:%s", client_config->ssid);

        connect_timeout_timer_args = {
            .callback = &connect_timeout_timer_callback,
            .arg = (void *)ctx,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_connect_timeout"};

        ESP_LOGI(TAG, "Creating WiFi connect timeout timer");
        ESP_ERROR_CHECK(esp_timer_create(&connect_timeout_timer_args, &connect_timeout_timer));
        ESP_LOGI(TAG, "Starting WiFi connect timeout timer");
        ESP_ERROR_CHECK(esp_timer_start_once(connect_timeout_timer, 30000000));
    }
    free(client_config);
}