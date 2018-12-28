#include "webserver.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>

#include "settings.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX_SCAN_APS 15
#define ERR_MSG_BLE_NOT_STARTED "BLE not yet started"

ESP_EVENT_DEFINE_BASE(WIFI_SCAN_EVENT)

static const char *TAG = "webserver";

#define INDEX_FILE "/spiffs/index.html"

#define EXAMPLE_MDNS_INSTANCE "ibbq"
//static const char c_config_hostname[] = "ibbq";

esp_event_loop_handle_t wifi_scan_loop;

TaskHandle_t scan_task_handle = NULL;
static SemaphoreHandle_t wifi_scan_semaphore = NULL;
//StaticSemaphore_t wifi_scan_semaphore_buffer;

typedef struct wifi_scan_data
{
    uint16_t scanned_aps_count;
    wifi_ap_record_t scanned_aps[MAX_SCAN_APS];
} wifi_scan_data_t;

static wifi_scan_data_t scanned_wifi_data = {};

static cJSON *serialize_system(ibbq_state_t *bbq_state)
{
    cJSON *system = cJSON_CreateObject();

    system_settings_t *sys_settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    loadSettings(SYSTEM_SETTINGS, sys_settings);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[12];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
    cJSON_AddStringToObject(system, "serial", serial);
    if (bbq_state)
    {
        cJSON_AddBoolToObject(system, "ibbq_connected", bbq_state->connected);
        cJSON_AddNumberToObject(system, "ibbq_rssi", bbq_state->rssi);
        cJSON_AddNumberToObject(system, "soc", bbq_state->battery_percent);
    }
    else
    {
        cJSON_AddBoolToObject(system, "ibbq_connected", false);
        cJSON_AddNumberToObject(system, "ibbq_rssi", 0);
        cJSON_AddNumberToObject(system, "soc", 0);
    }

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK)
    {
        cJSON_AddNumberToObject(system, "rssi", ap_info.rssi);
    }
    else if (err != ESP_ERR_WIFI_NOT_CONNECT)
    {
        ESP_LOGW(TAG, "Failed to retrieve WiFi info due to unknown error: %s", esp_err_to_name(err));
    }

    cJSON_AddStringToObject(system, "unit", sys_settings->unit);
    cJSON_AddStringToObject(system, "ap", sys_settings->ap_name);
    cJSON_AddStringToObject(system, "language", sys_settings->lang);
    cJSON_AddStringToObject(system, "hwversion", "iBBQ-Gateway dev");
    cJSON_AddStringToObject(system, "host", sys_settings->hostname);
    cJSON_AddBoolToObject(system, "autoupd", false);

    free(sys_settings);

    return system;
}

static void initialise_mdns(void)
{
    system_settings_t *sys_settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    loadSettings(SYSTEM_SETTINGS, sys_settings);

    ESP_LOGI(TAG, "Starting to announce our existence via mDNS");
    _Static_assert(sizeof(sys_settings->hostname) < CONFIG_MAIN_TASK_STACK_SIZE / 2, "Configured mDNS name consumes more than half of the stack. Please select a shorter host name or extend the main stack size please.");
    const size_t config_hostname_len = sizeof(sys_settings->hostname) - 1; // without term char
    char hostname[config_hostname_len + 1 + 3 * 2 + 1];                    // adding underscore + 3 digits + term char
    uint8_t mac[6];

    // adding 3 LSBs from mac addr to setup a board specific name
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "%s_%02x%02X%02X", sys_settings->hostname, mac[3], mac[4], mac[5]);

    //initialize mDNS
    ESP_ERROR_CHECK(mdns_init());
    //set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);
    //set default mDNS instance name
    ESP_ERROR_CHECK(mdns_instance_name_set(sys_settings->hostname));

    //structure with TXT records
    // TODO set usable values
    //mdns_txt_item_t serviceTxtData[1] = {
    //    {"board", "esp32"}};

    //initialize service
    ESP_ERROR_CHECK(mdns_service_add("ibbq-server", "_http", "_tcp", 80, NULL /*serviceTxtData*/, 0));
    //add another TXT item
    ESP_ERROR_CHECK(mdns_service_txt_item_set("_http", "_tcp", "path", "/"));
    //change TXT item value
    //ESP_ERROR_CHECK(mdns_service_txt_item_set("_http", "_tcp", "u", "admin"));
    free(sys_settings);
}

static esp_err_t file_handler(httpd_req_t *req)
{
    const char *fileName = (const char *)req->user_ctx;
    FILE *f = fopen(fileName, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to read file %s", fileName);
        return ESP_FAIL;
    }

    char buf[64];
    size_t readN = 0;
    do
    {
        readN = fread(buf, 1, sizeof(buf), f);
        esp_err_t ret = httpd_resp_send_chunk(req, buf, readN);
        if (ret != ESP_OK)
        {
            fclose(f);
            ESP_LOGE(TAG, "Failed to write to HTTP response: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
    } while (readN > 0);
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    ESP_LOGI(TAG, "Free heap after request: %d", esp_get_free_heap_size());
    return ESP_OK;
}

static httpd_uri_t index_route = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)INDEX_FILE};

static httpd_uri_t font_route = {
    .uri = "/nano.ttf",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)"/spiffs/nano.ttf"};

static httpd_uri_t fontello_route = {
    .uri = "/fontello.ttf",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)"/spiffs/fontello.ttf"};

static esp_err_t data_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON *system = serialize_system(bbq_state);
    cJSON_AddItemToObject(root, "system", system);

    cJSON *channels = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "channel", channels);
    if (bbq_state)
    {
        for (int i = 0; i < bbq_state->probe_count; i++)
        {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "number", i + 1);
            cJSON_AddStringToObject(data, "type", "iBBQ");
            cJSON_AddStringToObject(data, "name", bbq_state->probes[i].name);
            cJSON_AddNumberToObject(data, "temp", bbq_state->temps[i]);
            cJSON_AddNumberToObject(data, "min", bbq_state->probes[i].min);
            cJSON_AddNumberToObject(data, "max", bbq_state->probes[i].max);
            cJSON_AddStringToObject(data, "color", bbq_state->probes[i].color);
            //cJSON_AddBoolToObject(data, "alarm", bbq_state->probes[i].alarmEnabled);

            cJSON_AddItemToArray(channels, data);
        }
    }

    char *jsonString = NULL;
    jsonString = cJSON_Print(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, jsonString, strlen(jsonString));
    cJSON_Delete(root);
    free(jsonString);
    ESP_LOGI(TAG, "Free heap after request: %d", esp_get_free_heap_size());
    return ESP_OK;
}

static httpd_uri_t data_route = {
    .uri = "/data",
    .method = HTTP_GET,
    .handler = data_handler,
    .user_ctx = NULL};

static esp_err_t data_set_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;
    if (!bbq_state)
    {
        httpd_resp_set_status(req, "404");
        httpd_resp_send(req, ERR_MSG_BLE_NOT_STARTED, sizeof(ERR_MSG_BLE_NOT_STARTED));
        return ESP_OK;
    }
    char buf[2048];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf))
    {
        ESP_LOGW(TAG, "Received request with %d bytes, which is larger than buf with %d bytes", remaining, sizeof(buf));
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    if ((ret = httpd_req_recv(req, buf,
                              MIN(remaining, sizeof(buf)))) <= 0)
    {
        if (ret != HTTPD_SOCK_ERR_TIMEOUT)
        {
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Received settings: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (channels != NULL)
    {

        cJSON *channel = NULL;
        cJSON_ArrayForEach(channel, channels)
        {
            int probeId = 0;
            cJSON *probeNumber = cJSON_GetObjectItem(channel, "number");
            if (probeNumber != NULL && cJSON_IsNumber(probeNumber))
            {
                probeId = probeNumber->valueint;
            }
            else
            {
                ESP_LOGE(TAG, "Channel config does not have a valid probe number");
                httpd_resp_set_status(req, "400");
                httpd_resp_send_chunk(req, NULL, 0);
                cJSON_Delete(root);
                return ESP_OK;
            }
            cJSON *name = cJSON_GetObjectItem(channel, "name");
            if (cJSON_IsString(name))
            {
                strncpy(bbq_state->probes[probeId].name, name->valuestring, sizeof(bbq_state->probes[probeId].name));
                bbq_state->probes[probeId].name[sizeof(bbq_state->probes[probeId].name) - 1] = '\0';
            }

            cJSON *min = cJSON_GetObjectItem(channel, "min");
            if (cJSON_IsNumber(min))
            {
                bbq_state->probes[probeId].min = (float)min->valuedouble;
            }

            cJSON *max = cJSON_GetObjectItem(channel, "max");
            if (cJSON_IsNumber(max))
            {
                bbq_state->probes[probeId].max = (float)max->valuedouble;
            }

            cJSON *color = cJSON_GetObjectItem(channel, "color");
            if (cJSON_IsString(color))
            {
                strncpy(bbq_state->probes[probeId].color, color->valuestring, sizeof(bbq_state->probes[probeId].color));
                bbq_state->probes[probeId].color[sizeof(bbq_state->probes[probeId].color) - 1] = '\0';
            }

            // TODO parse alarm settings
        }
        saveSettings(CHANNEL_SETTINGS, bbq_state->probes);
    }
    cJSON_Delete(root);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t data_set_route = {
    .uri = "/data",
    .method = HTTP_POST,
    .handler = data_set_handler,
    .user_ctx = NULL};

static esp_err_t set_system_handler(httpd_req_t *req)
{
    system_settings *sys_settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    loadSettings(SYSTEM_SETTINGS, sys_settings);

    char buf[2048];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf))
    {
        ESP_LOGW(TAG, "Received request with %d bytes, which is larger than buf with %d bytes", remaining, sizeof(buf));
        httpd_resp_set_status(req, "400");
        return ESP_OK;
    }

    if ((ret = httpd_req_recv(req, buf,
                              MIN(remaining, sizeof(buf)))) <= 0)
    {
        if (ret != HTTPD_SOCK_ERR_TIMEOUT)
        {
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Received system settings: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (host != NULL && cJSON_IsString(host))
    {
        strncpy(sys_settings->hostname, host->valuestring, sizeof(sys_settings->hostname));
        sys_settings->hostname[sizeof(sys_settings->hostname) - 1] = '\0';
    }

    cJSON *unit = cJSON_GetObjectItemCaseSensitive(root, "unit");
    if (unit != NULL && cJSON_IsString(unit))
    {
        strncpy(sys_settings->unit, unit->valuestring, sizeof(sys_settings->unit));
        sys_settings->unit[sizeof(sys_settings->unit) - 1] = '\0';
    }

    cJSON *lang = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (lang != NULL && cJSON_IsString(lang))
    {
        strncpy(sys_settings->lang, lang->valuestring, sizeof(sys_settings->lang));
        sys_settings->lang[sizeof(sys_settings->lang) - 1] = '\0';
    }

    cJSON *ap_name = cJSON_GetObjectItemCaseSensitive(root, "ap_name");
    if (ap_name != NULL && cJSON_IsString(ap_name))
    {
        strncpy(sys_settings->ap_name, ap_name->valuestring, sizeof(sys_settings->ap_name));
        sys_settings->ap_name[sizeof(sys_settings->ap_name) - 1] = '\0';
    }

    cJSON_Delete(root);

    saveSettings(SYSTEM_SETTINGS, sys_settings);
    free(sys_settings);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t set_system_route = {
    .uri = "/setsystem",
    .method = HTTP_POST,
    .handler = set_system_handler,
    .user_ctx = NULL};

static esp_err_t get_system_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

    cJSON *system = serialize_system(bbq_state);

    char *jsonString = cJSON_Print(system);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, jsonString, strlen(jsonString));
    cJSON_Delete(system);
    free(jsonString);

    return ESP_OK;
}

static httpd_uri_t get_system_route = {

    .uri = "/setsystem",
    .method = HTTP_GET,
    .handler = set_system_handler,
    .user_ctx = NULL};

/*
{
    "system": {
        "host": "",
        "getupdate": "version",
        "ap": "ap-name",
        "unit": "celsius or fahrenheit",
        "charge": battery_percent,
        //"soc": int

    },
    "hardware": [
        "",
    ],
    "sensors": [
        {},
    ]
}
*/
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON *system = serialize_system(bbq_state);
    cJSON_AddItemToObject(root, "system", system);

    cJSON *sensors = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sensors", sensors);
    cJSON *sensorType = cJSON_CreateString("iBBQ");
    cJSON_AddItemToArray(sensors, sensorType);

    char *jsonString = NULL;
    jsonString = cJSON_Print(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, jsonString, strlen(jsonString));

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Returning settings: %s", jsonString);
    free(jsonString);

    return ESP_OK;
}

static httpd_uri_t settings_get_route = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
    .user_ctx = NULL};

void scan_task(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    ESP_LOGI(TAG, "Scanning for neighbouring access points");
    wifi_scan_data_t *ctx = (wifi_scan_data_t *)handler_args;
    if (ctx->scanned_aps_count > 0)
    {
        ctx->scanned_aps_count = 0;
    }
    wifi_scan_config_t config = {};
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err == ESP_ERR_WIFI_TIMEOUT)
    {
        ESP_LOGW(TAG, "WiFi timeout during scanning");
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Unexpected error during WiFi scanning: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ctx->scanned_aps_count));
    ESP_LOGI(TAG, "Found %d access points", ctx->scanned_aps_count);
    ctx->scanned_aps_count = MIN(ctx->scanned_aps_count, MAX_SCAN_APS);
    if (ctx->scanned_aps_count > 0)
    {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ctx->scanned_aps_count, ctx->scanned_aps));
    }
    xSemaphoreGive(wifi_scan_semaphore);
}

void initiate_wifi_scan()
{
    if (xSemaphoreTake(wifi_scan_semaphore, (TickType_t)10))
    {
        xSemaphoreGive(wifi_scan_semaphore);
        ESP_ERROR_CHECK(esp_event_post_to(wifi_scan_loop, WIFI_SCAN_EVENT, WIFI_SCAN_REQUESTED, NULL, 0, portMAX_DELAY));
    }
    else
    {
        ESP_LOGI(TAG, "WiFi scan already in process");
    }
}

static esp_err_t networkscan_handler(httpd_req_t *req)
{
    initiate_wifi_scan();

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t networkscan_route = {
    .uri = "/networkscan",
    .method = HTTP_GET,
    .handler = networkscan_handler,
    .user_ctx = NULL};

static esp_err_t networklist_handler(httpd_req_t *req)
{
    wifi_scan_data_t *ctx = (wifi_scan_data_t *)req->user_ctx;
    wifi_ap_record_t *records = NULL;
    uint16_t ap_count = 0;
    bool semaphore_taken = xSemaphoreTake(wifi_scan_semaphore, (TickType_t)5);
    if (semaphore_taken)
    {
        ESP_LOGI(TAG, "WiFi scan seems to be finished, accessing data");
        records = ctx->scanned_aps;
        ap_count = ctx->scanned_aps_count;
    }
    ap_count = MIN(ap_count, MAX_SCAN_APS);
    ESP_LOGI(TAG, "Serializing %d access point records", ap_count);
    cJSON *root = cJSON_CreateObject();
    cJSON *scan = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "Scan", scan);

    for (int i = 0; i < ap_count; i++)
    {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "SSID", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "RSSI", records[i].rssi);
        cJSON_AddNumberToObject(ap, "Enc", records[i].authmode);
        // TODO add encryption info
        cJSON_AddItemToArray(scan, ap);
    }
    char *jsonString = NULL;
    jsonString = cJSON_Print(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, jsonString, strlen(jsonString));

    cJSON_Delete(root);
    if (semaphore_taken)
    {
        ESP_LOGI(TAG, "Releasing semaphore for WiFi scan after listing available WiFis");
        xSemaphoreGive(wifi_scan_semaphore);
    }
    return ESP_OK;
}

static httpd_uri_t networklist_route = {
    .uri = "/networklist",
    .method = HTTP_GET,
    .handler = networklist_handler,
    .user_ctx = NULL};

static esp_err_t setnetwork_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf))
    {
        ESP_LOGW(TAG, "Received request with %d bytes, which is larger than buf with %d bytes", remaining, sizeof(buf));
        httpd_resp_set_status(req, "400");
        return ESP_OK;
    }

    if ((ret = httpd_req_recv(req, buf,
                              MIN(remaining, sizeof(buf)))) <= 0)
    {
        if (ret != HTTPD_SOCK_ERR_TIMEOUT)
        {
            return ESP_FAIL;
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    wifi_client_config_t *wifi_config = (wifi_client_config_t *)malloc(sizeof(wifi_client_config_t));
    loadSettings(WIFI_SETTINGS, wifi_config);

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (ssid != NULL && cJSON_IsString(ssid))
    {
        strncpy(wifi_config->ssid, ssid->valuestring, 32);
        wifi_config->ssid[31] = '\0';
    }
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (password != NULL && cJSON_IsString(password))
    {
        strncpy(wifi_config->psk, password->valuestring, 64);
        wifi_config->ssid[63] = '\0';
    }

    saveSettings(WIFI_SETTINGS, wifi_config);
    // TODO apply these settings
    httpd_resp_send_chunk(req, NULL, 0);
    cJSON_Delete(root);
    free(wifi_config);
    esp_restart();
    return ESP_OK;
}

static httpd_uri_t setnetwork_route{
    .uri = "/setnetwork",
    .method = HTTP_POST,
    .handler = setnetwork_handler,
    .user_ctx = NULL};

static esp_err_t setchannels_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;
    if (!bbq_state)
    {
        httpd_resp_set_status(req, "404");
        httpd_resp_send(req, ERR_MSG_BLE_NOT_STARTED, sizeof(ERR_MSG_BLE_NOT_STARTED));
        return ESP_OK;
    }

    char buf[1024];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf))
    {
        ESP_LOGW(TAG, "Received request with %d bytes, which is larger than buf with %d bytes", remaining, sizeof(buf));
        httpd_resp_set_status(req, "400");
        return ESP_OK;
    }

    if ((ret = httpd_req_recv(req, buf,
                              MIN(remaining, sizeof(buf)))) <= 0)
    {
        if (ret != HTTPD_SOCK_ERR_TIMEOUT)
        {
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Received channel config: %s", buf);
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    size_t probeId = 0;

    cJSON *number = cJSON_GetObjectItemCaseSensitive(root, "number");
    if (number != NULL && cJSON_IsNumber(number) && number->valueint > 0)
    {
        probeId = number->valueint - 1;
    }
    else
    {
        httpd_resp_set_status(req, "400");
        httpd_resp_send_chunk(req, NULL, 0);
    }

    probe_data_t *probe_data = &bbq_state->probes[probeId];

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (name != NULL && cJSON_IsString(name))
    {
        strncpy(probe_data->name, name->valuestring, sizeof(probe_data->name));
        probe_data->name[sizeof(probe_data->name) - 1] = '\0';
    }

    cJSON *min = cJSON_GetObjectItemCaseSensitive(root, "min");
    if (min != NULL && cJSON_IsNumber(min))
    {
        probe_data->min = min->valueint;
    }

    cJSON *max = cJSON_GetObjectItemCaseSensitive(root, "max");
    if (max != NULL && cJSON_IsNumber(max))
    {
        probe_data->max = max->valueint;
    }

    cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    if (color != NULL && cJSON_IsString(color))
    {
        strncpy(probe_data->color, color->valuestring, sizeof(probe_data->color));
        probe_data->color[sizeof(probe_data->color) - 1] = '\0';
    }

    // TODO handle alarms

    ESP_LOGI(TAG, "Updating probe %d with name %s, min %f, max %f and color %s",
             probeId,
             bbq_state->probes[probeId].name,
             bbq_state->probes[probeId].min,
             bbq_state->probes[probeId].max,
             bbq_state->probes[probeId].color);

    saveSettings(CHANNEL_SETTINGS, bbq_state->probes);

    cJSON_Delete(root);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static httpd_uri_t setchannels_route = {
    .uri = "/setchannels",
    .method = HTTP_POST,
    .handler = setchannels_handler,
    .user_ctx = NULL};

httpd_handle_t init_webserver(ibbq_state_t *state)
{
    static httpd_handle_t server = NULL;
    static httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    wifi_scan_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(wifi_scan_semaphore);

    esp_event_loop_args_t wifi_scan_loop_args = {
        .queue_size = 5,
        .task_name = "wifi_scan_loop_task", // task will be created
        .task_priority = uxTaskPriorityGet(NULL),
        .task_stack_size = 2048,
        .task_core_id = tskNO_AFFINITY};

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    esp_err_t ret = httpd_start(&server, &config);
    if (ret == ESP_OK)
    {
        scanned_wifi_data.scanned_aps_count = 0;
        ESP_ERROR_CHECK(esp_event_loop_create(&wifi_scan_loop_args, &wifi_scan_loop));
        ESP_ERROR_CHECK(esp_event_handler_register_with(wifi_scan_loop, WIFI_SCAN_EVENT, WIFI_SCAN_REQUESTED, scan_task, &scanned_wifi_data));
        initiate_wifi_scan();

        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &index_route);
        httpd_register_uri_handler(server, &font_route);
        httpd_register_uri_handler(server, &fontello_route);
        data_route.user_ctx = (void *)state;
        httpd_register_uri_handler(server, &data_route);
        data_set_route.user_ctx = (void *)state;
        httpd_register_uri_handler(server, &data_set_route);
        settings_get_route.user_ctx = (void *)state;
        httpd_register_uri_handler(server, &settings_get_route);
        setchannels_route.user_ctx = (void *)state;
        httpd_register_uri_handler(server, &setchannels_route);
        httpd_register_uri_handler(server, &set_system_route);
        get_system_route.user_ctx = (void *)state;
        httpd_register_uri_handler(server, &get_system_route);
        networklist_route.user_ctx = (void *)&scanned_wifi_data;
        httpd_register_uri_handler(server, &networklist_route);
        networkscan_route.user_ctx = (void *)&scanned_wifi_data;
        httpd_register_uri_handler(server, &networkscan_route);
        httpd_register_uri_handler(server, &setnetwork_route);
        initialise_mdns();
        return server;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start webserver: %s", esp_err_to_name(ret));
        esp_restart();
    }
    return NULL;
}

void stop_webserver(httpd_handle_t server)
{
    ESP_LOGD(TAG, "Freesing mDNS");
    mdns_free();
    // Stop the httpd server
    ESP_LOGD(TAG, "Stopping httpd");
    esp_err_t ret = httpd_stop(server);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop httpd server: %s", esp_err_to_name(ret));
    }
}