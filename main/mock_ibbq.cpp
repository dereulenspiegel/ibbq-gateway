#include "ibbq.h"

#ifdef MOCK_IBBQ

#include "esp_log.h"
#include "esp_timer.h"

#define MOCK_REFRESH_INTERVAL 1000000

static const char *TAG = "mock-ibbq";

static ibbq_state ctx = {};

static void mock_timer_callback(void *arg);
static esp_timer_handle_t mock_timer;

const esp_timer_create_args_t mock_timer_args = {
    .callback = &mock_timer_callback,
    /* argument specified here will be passed to timer callback function */
    .arg = (void *)&ctx,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "mock_ibbq"};

static void mock_timer_callback(void *arg)
{
    ibbq_state_t *bbq_state = (ibbq_state_t *)arg;
    bbq_state->connected = true;
    bbq_state->probe_count = MAX_PROBE_COUNT;
    bbq_state->battery_percent = 52.0f;
    bbq_state->rssi = 12;

    for (int i = 0; i < MAX_PROBE_COUNT; i++)
    {
        bbq_state->temps[i] = i * 13;
    }
}

ibbq_state_t *init_ibbq()
{

    ESP_LOGI(TAG, "Starting mock");
    ESP_ERROR_CHECK(esp_timer_create(&mock_timer_args, &mock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(mock_timer, MOCK_REFRESH_INTERVAL));
    return &ctx;
}

#endif