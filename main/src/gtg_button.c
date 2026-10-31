#include "gtg_button.h"

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "project_config.h"
#include "gtg_events.h"

#define BUTTON_TASK_STACK_SIZE  2560
#define BUTTON_TASK_PRIORITY    6

static const char *TAG = "button";

static TaskHandle_t s_task = NULL;

/* Only wakes the task, the timing and posting happen there */
static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

static bool is_pushed(void)
{
    return gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
}

static void post_button_event(gtg_event_type_t type)
{
    gtg_event_t event = {
        .type = type,
        .button.id = GTG_BUTTON_ID_MAIN,
    };
    gtg_events_post(&event);
}

/* A click or short hold, decided when the button is released before the long hold time */
static void post_release(int64_t held_ms)
{
    ESP_LOGD(TAG, "released after %lld ms", held_ms);
    post_button_event(held_ms >= GTG_BUTTON_SHORT_HOLD_MS ? GTG_EV_BUTTON_SHORT_HOLD : GTG_EV_BUTTON_CLICKED);
}

/* Ticks left until the push reaches the long hold time, rounded up so it isn't reached early */
static TickType_t ticks_until_long_hold(int64_t pushed_at)
{
    int64_t remaining_ms = GTG_BUTTON_LONG_HOLD_MS - (esp_timer_get_time() - pushed_at) / 1000;
    return remaining_ms > 0 ? pdMS_TO_TICKS(remaining_ms) + 1 : 0;
}

/*
 * Each edge wakes the task, which waits out the bounce and then reads the level. Edges during
 * the wait are dropped before reading, so a bounce only counts as one change, while an edge
 * after the read wakes the task again. A push is timed from its debounced start.
 *
 * While the button is pushed the task also wakes at the long hold time, and posts the long hold
 * right then without waiting for the release. The release after a long hold posts nothing, any
 * other release posts a click or a short hold.
 */
static void button_task(void *arg)
{
    bool pushed = is_pushed();
    bool long_hold_posted = false;
    int64_t pushed_at = esp_timer_get_time();

    if (pushed) {
        ESP_LOGW(TAG, "pushed at start, timing the push from now");
    }

    while (true) {
        TickType_t wait = (pushed && !long_hold_posted) ? ticks_until_long_hold(pushed_at) : portMAX_DELAY;

        if (ulTaskNotifyTake(pdTRUE, wait) == 0) {
            /* no edge within the wait, so the button has been pushed for the long hold time */
            if (pushed && !long_hold_posted) {
                long_hold_posted = true;
                post_button_event(GTG_EV_BUTTON_LONG_HOLD);
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(GTG_BUTTON_DEBOUNCE_MS));
        ulTaskNotifyTake(pdTRUE, 0);

        bool now_pushed = is_pushed();
        if (now_pushed == pushed) {
            /* a bounce or a glitch that settled back */
            continue;
        }
        pushed = now_pushed;

        if (pushed) {
            pushed_at = esp_timer_get_time();
            long_hold_posted = false;
        } else if (!long_hold_posted) {
            post_release((esp_timer_get_time() - pushed_at) / 1000);
        }
    }
}

esp_err_t gtg_button_init(void)
{
    if (s_task != NULL) {
        ESP_LOGE(TAG, "already started");
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "failed to configure GPIO %d", BUTTON_GPIO);

    /* the task must exist before the interrupt can wake it */
    if (xTaskCreate(button_task, "gtg button", BUTTON_TASK_STACK_SIZE, NULL,
                    BUTTON_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* the service is shared by all GPIO interrupts, another module may have installed it */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "failed to install the GPIO interrupt service");
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    err = gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to add the button interrupt");
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "button on GPIO %d ready", BUTTON_GPIO);

    return ESP_OK;
}
