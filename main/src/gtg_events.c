#include "gtg_events.h"

#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "events";

static QueueHandle_t s_queue = NULL;

esp_err_t gtg_events_init(void)
{
    if (s_queue != NULL) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(GTG_EVENTS_QUEUE_LEN, sizeof(gtg_event_t));
    return s_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gtg_events_post(const gtg_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        ESP_LOGW(TAG, "not initialized, %s dropped", gtg_event_type_name(event->type));
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, %s dropped", gtg_event_type_name(event->type));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t IRAM_ATTR gtg_events_post_from_isr(const gtg_event_t *event, BaseType_t *higher_priority_woken)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueSendFromISR(s_queue, event, higher_priority_woken) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gtg_events_receive(gtg_event_t *event, TickType_t ticks)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueReceive(s_queue, event, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char *gtg_event_type_name(gtg_event_type_t type)
{
    switch (type) {
    case GTG_EV_CONNECTION_CHANGED: return "connection changed";
    case GTG_EV_API_KEY_REJECTED:   return "API key rejected";
    case GTG_EV_BUTTON_CLICKED:     return "button clicked";
    case GTG_EV_BUTTON_SHORT_HOLD:  return "button short hold";
    case GTG_EV_BUTTON_LONG_HOLD:   return "button long hold";
    case GTG_EV_CONFIG_APPLIED:     return "config applied";
    default:                        return "unknown";
    }
}

void gtg_events_post_key_rejected(gtg_api_key_t which)
{
    gtg_event_t event = {
        .type = GTG_EV_API_KEY_REJECTED,
        .api_key.which = which,
    };
    gtg_events_post(&event);
}
