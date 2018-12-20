#include "ibbq.h"

#ifndef MOCK_IBBQ
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <map>

#include "esp_log.h"
#include "ibbq_events.h"
#include "esp_event_base.h"
#include "esp_timer.h"

#define MAX_VOLTAGE 6550
#define BATTERY_INTERVAL 30000000

static const char *TAG = "iBBQ-BLE";

// The remote service we wish to connect to.
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
//static BLEUUID serviceUUID("00001800-0000-1000-8000-00805f9b34fb");
// The characteristic of the remote service we are interested in.
static BLEUUID accountVerifyUUID("0000fff2-0000-1000-8000-00805f9b34fb");
static BLEUUID realtimeData("0000fff4-0000-1000-8000-00805f9b34fb");
static BLEUUID settings("0000fff5-0000-1000-8000-00805f9b34fb");
static BLEUUID settingsResult("0000fff1-0000-1000-8000-00805f9b34fb");
static BLEUUID historyData("0000fff3-0000-1000-8000-00805f9b34fb");

static uint8_t credentials[] = {0x21, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0xb8, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t enableRealTimeData[] = {0x0B, 0x01, 0x00, 0x00, 0x00, 0x00};
static uint8_t unitCelsius[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t batteryLevel[] = {0x08, 0x24, 0x00, 0x00, 0x00, 0x00};

ESP_EVENT_DEFINE_BASE(IBBQ_EVENTS)

esp_event_loop_handle_t ble_loop;

static ibbq_state_t ctx = {};

static void battery_timer_callback(void *arg);
static esp_timer_handle_t battery_timer;

static esp_timer_create_args_t battery_timer_args = {
    .callback = &battery_timer_callback,
    /* argument specified here will be passed to timer callback function */
    .arg = (void *)&ctx,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "request_battery"};

static void battery_timer_callback(void *arg)
{
    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_REQUEST_STATE, NULL, 0, portMAX_DELAY));
}

static void start_battery_timer(ibbq_state_t *ctx)
{
    ESP_ERROR_CHECK(esp_timer_start_once(battery_timer, BATTERY_INTERVAL));
}

uint16_t littleEndianInt(uint8_t *pData)
{
    uint16_t val = pData[1] << 8;
    val = val | pData[0];
    return val;
}

static void realtimeDataCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
    ctx.probe_count = length / 2;

    uint8_t probeId = 0;
    for (int i = 0; i < length; i += 2)
    {
        uint16_t val = littleEndianInt(&pData[i]);
        float temp = val / 10;
        ESP_LOGI(TAG, "Probe %d has value %f", probeId, temp);
        ctx.temps[i] = temp;
        probeId++;
    }
}

static void settingsResultCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
    if (length < 5)
    {
        ESP_LOGE(TAG, "Length of received settings is too short");
        return;
    }
    uint16_t voltage = littleEndianInt(&pData[1]);
    uint16_t maxVoltage = littleEndianInt(&pData[3]);

    if (maxVoltage == 0)
    {
        maxVoltage = MAX_VOLTAGE;
    }
    ctx.battery_percent = (100 * voltage) / maxVoltage;
    ESP_LOGI(TAG, "Current battery level %f %%", ctx.battery_percent);
}

bool readSettings(BLEClient *pClient)
{
    BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == NULL)
    {
        ESP_LOGE(TAG, "Failed to get remote service for reading settings");
        return false;
    }

    BLERemoteCharacteristic *pRemoteCharacteristic = pRemoteService->getCharacteristic(settings);
    if (pRemoteCharacteristic == NULL)
    {
        ESP_LOGE(TAG, "Failed to get characteristic to read settings");
        return false;
    }

    // TODO probably return value???
    std::string value = pRemoteCharacteristic->readValue();
    return true;
}

bool writeSetting(BLEClient *pClient, uint8_t *data, size_t length)
{
    readSettings(pClient);
    BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == NULL)
    {
        ESP_LOGE(TAG, "Failed to find our login service UUID: %s", serviceUUID.toString().c_str());
        return false;
    }

    BLERemoteCharacteristic *pRemoteCharacteristic = pRemoteService->getCharacteristic(settings);
    if (pRemoteCharacteristic == nullptr)
    {
        ESP_LOGE(TAG, "Failed to find our settings characteristic");
        return false;
    }

    if (!pRemoteCharacteristic->canWrite())
    {
        ESP_LOGE(TAG, "Settings characteristic can't be written to");
        return false;
    }

    pRemoteCharacteristic->writeValue(data, length, false);
    return true;
}

bool requestBatteryLevel(BLEClient *pClient)
{
    ESP_LOGI(TAG, "Requesting battery level");
    return writeSetting(pClient, batteryLevel, sizeof(batteryLevel));
}

bool subscribeToCharacteristic(BLEClient *pClient, BLEUUID uuid, void (*notifyCallback)(BLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify))
{
    BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == NULL)
    {
        ESP_LOGE(TAG, "Failed to find our service UUID: %s", serviceUUID.toString().c_str());
        return false;
    }

    BLERemoteCharacteristic *pRemoteCharacteristic = pRemoteService->getCharacteristic(uuid);
    if (pRemoteCharacteristic == nullptr)
    {
        ESP_LOGE(TAG, "Failed to find our characteristic");
        return false;
    }

    if (!pRemoteCharacteristic->canNotify())
    {
        ESP_LOGE(TAG, "characteristic can not notify");
        return false;
    }

    std::string value = pRemoteCharacteristic->readValue();
    pRemoteCharacteristic->registerForNotify(notifyCallback);

    // const uint8_t bothOn[]         = {0x3, 0x0};
    const uint8_t notificationOn[] = {0x1, 0x0};
    pRemoteCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)notificationOn, 2, true);

    return true;
}

class MyBLECLientCallbacks : public BLEClientCallbacks
{
    void onConnect(BLEClient *pClient)
    {
        ESP_LOGI(TAG, "iBBQ BLE client connected callback");
    }

    void onDisconnect(BLEClient *pClient)
    {
        //pClient->disconnect();
        esp_timer_stop(battery_timer);
        ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_START_SCAN, NULL, 0, portMAX_DELAY));
    }
};

static MyBLECLientCallbacks *clientCallbacks = new MyBLECLientCallbacks();

void ble_scan_finished(BLEScanResults results)
{
    ESP_LOGI(TAG, "Discovered %d BLE devices", results.getCount());
    for (uint32_t i = 0; i < results.getCount(); i++)
    {
        BLEAdvertisedDevice dev = results.getDevice(i);
        if (dev.getName() == "iBBQ")
        {
            //dev.getScan()->stop();
            ESP_LOGI(TAG, "Found iBBQ device (%s)", dev.getAddress().toString().c_str());
            ESP_LOGI(TAG, "Remote service UUID is %s", dev.getServiceUUID().toString().c_str());
            ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_DISCOVERED, &dev, sizeof(dev), portMAX_DELAY));
            return;
        }
    }
    ESP_LOGI(TAG, "No iBBQ device found, posting scan event again");
    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_START_SCAN, NULL, 0, portMAX_DELAY));
}

static void device_discovered(void *handler_args, esp_event_base_t base, int32_t id, void *event_dat)
{
    ibbq_state_t *ctx = (ibbq_state_t *)handler_args;
    BLEAdvertisedDevice *dev = (BLEAdvertisedDevice *)event_dat;

    //ESP_LOGI(TAG, "Discovered device: %s", dev->toString().c_str());

    ESP_LOGI(TAG, "Connecting to device address (%s)", dev->getAddress().toString().c_str());

    if (!ctx->pClient->connect(dev->getAddress()))
    {
        ESP_LOGE(TAG, "Failed to connect to device %s", dev->getAddress().toString().c_str());
    }
    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_CONNECTED, NULL, 0, portMAX_DELAY));
}

static void device_connected(void *handler_args, esp_event_base_t base, int32_t id, void *event_dat)
{
    ibbq_state_t *ctx = (ibbq_state_t *)handler_args;
    if (!ctx->pClient->isConnected())
    {
        ESP_LOGE(TAG, "Connection doesn't seem to be established");
    }
    else
    {
        ESP_LOGI(TAG, "Client seems to be connected");
    }

    /*std::map<std::string, BLERemoteService *>::iterator serviceIt;
    for (serviceIt = ctx->pClient->getServices()->begin(); serviceIt != ctx->pClient->getServices()->end(); serviceIt++)
    {
        ESP_LOGI(TAG, "Found service %s: %s", serviceIt->first.c_str(), serviceIt->second->toString().c_str());
    }*/

    ESP_LOGI(TAG, "Authenticating against iBBQ");
    BLERemoteService *pRemoteService = ctx->pClient->getService(serviceUUID);
    if (pRemoteService == NULL)
    {
        ESP_LOGE(TAG, "Failed to get remote service for login");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Found service for iBBQ login");
    /*std::map<std::string, BLERemoteCharacteristic *>::iterator it;
    for (it = pRemoteService->getCharacteristics()->begin(); it != pRemoteService->getCharacteristics()->end(); it++)
    {
        ESP_LOGI(TAG, "Characteristic %s: %s", it->first.c_str(), it->second->toString().c_str());
    }*/

    BLERemoteCharacteristic *pRemoteCharacteristic = pRemoteService->getCharacteristic(accountVerifyUUID);
    if (pRemoteCharacteristic == NULL)
    {
        ESP_LOGE(TAG, "Failed to find our characteristic");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Found characteristic for iBBQ login");

    if (!pRemoteCharacteristic->canWrite())
    {
        ESP_LOGE(TAG, "AccountAndVerify characteristic is not writeable");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Found our characteristic, writing credentials");
    pRemoteCharacteristic->writeValue(credentials, sizeof(credentials), false);
    ESP_LOGI(TAG, "Authentication with iBBQ was successfull");
    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_AUTHENTICATED, NULL, 0, portMAX_DELAY));
}

static void device_authenticated(void *handler_args, esp_event_base_t base, int32_t id, void *event_dat)
{
    ibbq_state_t *ctx = (ibbq_state_t *)handler_args;

    ESP_LOGI(TAG, "Setting units to Celsius");
    if (!writeSetting(ctx->pClient, unitCelsius, sizeof(unitCelsius)))
    {
        ESP_LOGE(TAG, "Failed to set units to celsius");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Subscribing to realtime data");
    if (!subscribeToCharacteristic(ctx->pClient, realtimeData, realtimeDataCallback))
    {
        ESP_LOGE(TAG, "Failed to subscribe to realtime data");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Enabling realtime data");
    if (!writeSetting(ctx->pClient, enableRealTimeData, sizeof(enableRealTimeData)))
    {
        ESP_LOGE(TAG, "Failed to enable realtime data");
        ctx->pClient->disconnect();
        return;
    }
    ESP_LOGI(TAG, "Subscribing to settings");
    if (!subscribeToCharacteristic(ctx->pClient, settingsResult, settingsResultCallback))
    {
        ESP_LOGE(TAG, "Failed to subscribe to settings/battery result");
        ctx->pClient->disconnect();
        return;
    }
    ctx->connected = true;
    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_REQUEST_STATE, ctx, sizeof(*ctx), portMAX_DELAY));
}

static void request_device_state(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    ESP_LOGI(TAG, "Free heap after during BLE operation: %d", esp_get_free_heap_size());
    ibbq_state_t *ctx = (ibbq_state_t *)handler_args;
    ctx->rssi = ctx->pClient->getRssi();
    if (!writeSetting(ctx->pClient, batteryLevel, sizeof(batteryLevel)))
    {
        ESP_LOGE(TAG, "Failed to request device status like battery");
        ctx->pClient->disconnect();
        return;
    }

    start_battery_timer(ctx);
}

static void start_discovery_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    ibbq_state_t *state = (ibbq_state_t *)handler_args;
    state->connected = false;
    state->probe_count = 0;
    //state->pBLEScan->setAdvertisedDeviceCallbacks(scanCallback);
    state->pBLEScan->clearResults();
    state->pBLEScan->start(5, ble_scan_finished, false);
    ESP_LOGI(TAG, "Starting discovery of iBBQ devices");
}

ibbq_state_t *init_ibbq()
{
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "Initialising BLE for iBBQ");
    BLEDevice::init("");

    ctx.pBLEScan = BLEDevice::getScan();
    ctx.pBLEScan->setActiveScan(true);

    ctx.pClient = BLEDevice::createClient();
    ctx.pClient->setClientCallbacks(clientCallbacks);

    esp_event_loop_args_t ble_loop_args = {
        .queue_size = 5,
        .task_name = "ble_task", // task will be created
        .task_priority = uxTaskPriorityGet(NULL),
        .task_stack_size = 4096,
        .task_core_id = tskNO_AFFINITY};

    ESP_ERROR_CHECK(esp_timer_create(&battery_timer_args, &battery_timer));

    ESP_ERROR_CHECK(esp_event_loop_create(&ble_loop_args, &ble_loop));
    ESP_ERROR_CHECK(esp_event_handler_register_with(ble_loop, IBBQ_EVENTS, IBBQ_START_SCAN, start_discovery_handler, &ctx));
    ESP_ERROR_CHECK(esp_event_handler_register_with(ble_loop, IBBQ_EVENTS, IBBQ_DISCOVERED, device_discovered, &ctx));
    ESP_ERROR_CHECK(esp_event_handler_register_with(ble_loop, IBBQ_EVENTS, IBBQ_CONNECTED, device_connected, &ctx));
    ESP_ERROR_CHECK(esp_event_handler_register_with(ble_loop, IBBQ_EVENTS, IBBQ_AUTHENTICATED, device_authenticated, &ctx));
    ESP_ERROR_CHECK(esp_event_handler_register_with(ble_loop, IBBQ_EVENTS, IBBQ_REQUEST_STATE, request_device_state, &ctx));

    ESP_ERROR_CHECK(esp_event_post_to(ble_loop, IBBQ_EVENTS, IBBQ_START_SCAN, &ctx, sizeof(ibbq_state_t), portMAX_DELAY));

    return &ctx;
}
#endif