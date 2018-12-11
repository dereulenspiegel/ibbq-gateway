#include "webserver.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>

#include "settings.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *TAG = "webserver";

#define INDEX_FILE "/spiffs/index.html"

#define EXAMPLE_MDNS_INSTANCE "ibbq"
//static const char c_config_hostname[] = "ibbq";

static cJSON *serialize_system(ibbq_state_t *bbq_state)
{
    cJSON *system = cJSON_CreateObject();

    wifi_ap_record_t ap_info;
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));

    system_settings_t *sys_settings = (system_settings_t *)malloc(sizeof(system_settings_t));
    loadSettings(SYSTEM_SETTINGS, sys_settings);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[12];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
    cJSON_AddStringToObject(system, "serial", serial);
    cJSON_AddBoolToObject(system, "ibbq_connected", bbq_state->connected);
    cJSON_AddNumberToObject(system, "ibbq_rssi", bbq_state->rssi);
    cJSON_AddNumberToObject(system, "soc", bbq_state->battery_percent);
    cJSON_AddNumberToObject(system, "rssi", ap_info.rssi);
    cJSON_AddStringToObject(system, "unit", sys_settings->unit);
    cJSON_AddStringToObject(system, "ap", sys_settings->ap_name);
    cJSON_AddStringToObject(system, "language", sys_settings->lang);
    cJSON_AddStringToObject(system, "hwversion", "iBBQ-Gateway dev");
    cJSON_AddStringToObject(system, "host", sys_settings->hostname);
    cJSON_AddBoolToObject(system, "autoupd", false);

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
    ESP_ERROR_CHECK(mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE));

    //structure with TXT records
    // TODO set usable values
    //mdns_txt_item_t serviceTxtData[1] = {
    //    {"board", "esp32"}};

    //initialize service
    ESP_ERROR_CHECK(mdns_service_add("ibbq-server", "_http", "_tcp", 80, NULL /*serviceTxtData*/, 1));
    //add another TXT item
    ESP_ERROR_CHECK(mdns_service_txt_item_set("_http", "_tcp", "path", "/"));
    //change TXT item value
    //ESP_ERROR_CHECK(mdns_service_txt_item_set("_http", "_tcp", "u", "admin"));
    free(sys_settings);
}

esp_err_t file_handler(httpd_req_t *req)
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

httpd_uri_t index_route = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)INDEX_FILE};

httpd_uri_t font_route = {
    .uri = "/nano.ttf",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)"/spiffs/nano.ttf"};

httpd_uri_t fontello_route = {
    .uri = "/fontello.ttf",
    .method = HTTP_GET,
    .handler = file_handler,
    .user_ctx = (char *)"/spiffs/fontello.ttf"};

esp_err_t data_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON *system = serialize_system(bbq_state);
    cJSON_AddItemToObject(root, "system", system);

    cJSON *channels = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "channel", channels);
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

    char *jsonString = NULL;
    jsonString = cJSON_Print(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, jsonString, strlen(jsonString));
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Returning JSON for probe data: %s", jsonString);
    free(jsonString);
    ESP_LOGI(TAG, "Free heap after request: %d", esp_get_free_heap_size());
    return ESP_OK;
}

httpd_uri_t data_route = {
    .uri = "/data",
    .method = HTTP_GET,
    .handler = data_handler,
    .user_ctx = NULL};

esp_err_t data_set_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;
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

httpd_uri_t data_set_route = {
    .uri = "/data",
    .method = HTTP_POST,
    .handler = data_set_handler,
    .user_ctx = NULL};

esp_err_t set_system_handler(httpd_req_t *req)
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

httpd_uri_t set_system_route = {
    .uri = "/setsystem",
    .method = HTTP_POST,
    .handler = set_system_handler,
    .user_ctx = NULL};

esp_err_t get_system_handler(httpd_req_t *req)
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

httpd_uri_t get_system_route = {

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
esp_err_t settings_get_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

    system_settings_t *settings = (system_settings_t *)malloc(sizeof(system_settings_t));

    loadSettings(SYSTEM_SETTINGS, settings);

    cJSON *root = cJSON_CreateObject();
    cJSON *system = serialize_system(bbq_state);
    cJSON_AddItemToObject(root, "system", system);
    cJSON_AddStringToObject(system, "host", settings->hostname);
    cJSON_AddStringToObject(system, "unit", settings->unit);

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

httpd_uri_t settings_get_route = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
    .user_ctx = NULL};

esp_err_t setchannels_handler(httpd_req_t *req)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)req->user_ctx;

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

httpd_uri_t setchannels_route = {
    .uri = "/setchannels",
    .method = HTTP_POST,
    .handler = setchannels_handler,
    .user_ctx = NULL};

httpd_handle_t init_webserver(ibbq_state_t *state)
{
    static httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    esp_err_t ret = httpd_start(&server, &config);
    if (ret == ESP_OK)
    {
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
        //initialise_mdns();
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
    mdns_free();
    // Stop the httpd server
    esp_err_t ret = httpd_stop(server);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop httpd server: %s", esp_err_to_name(ret));
    }
}