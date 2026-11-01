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

/* Ticks left until ms have passed since since, rounded up so it isn't reached early */
static TickType_t ticks_until(int64_t since, int64_t ms)
{
    int64_t remaining_ms = ms - (esp_timer_get_time() - since) / 1000;
    return remaining_ms > 0 ? pdMS_TO_TICKS(remaining_ms) + 1 : 0;
}

/* Posts the clicks of a sequence that ended before the triple click, one event each */
static void post_clicks(uint8_t clicks)
{
    for (uint8_t i = 0; i < clicks; i++) {
        post_button_event(GTG_EV_BUTTON_CLICKED);
    }
}

/*
 * Each edge wakes the task, which waits out the bounce and then reads the level. Edges during
 * the wait are dropped before reading, so a bounce only counts as one change, while an edge
 * after the read wakes the task again. A push is timed from its debounced start.
 *
 * While the button is pushed the task also wakes at the hold times, and posts the hold right then
 * without waiting for the release: first the short hold, then, still pushed, the long hold. The
 * release after a hold posts nothing.
 *
 * Clicks are counted in a sequence, each pushed within GTG_BUTTON_MULTI_CLICK_MS of the last one's
 * release. The third click posts the triple click on its release. A sequence that ends before
 * that, with no push within the window, posts its one or two clicks. A push of the sequence that
 * becomes a hold drops the clicks before it, only the hold is posted.
 */
static void button_task(void *arg)
{
    bool pushed = is_pushed();
    int64_t pushed_at = esp_timer_get_time();

    /* the holds of the current push that have been posted, a hold posts each once */
    bool short_hold_posted = false;
    bool long_hold_posted = false;

    /* clicks released in the current sequence, waiting for a next push within the window */
    uint8_t clicks = 0;
    int64_t released_at = 0;

    if (pushed) {
        /* e.g. the push that woke the device from deep sleep, which isn't a click. Marked as
         * handled, so neither its release nor a hold posts anything */
        ESP_LOGI(TAG, "pushed at start, ignoring this push");
        short_hold_posted = true;
        long_hold_posted = true;
    }

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (pushed && !short_hold_posted) {
            wait = ticks_until(pushed_at, GTG_BUTTON_SHORT_HOLD_MS);
        } else if (pushed && !long_hold_posted) {
            wait = ticks_until(pushed_at, GTG_BUTTON_LONG_HOLD_MS);
        } else if (!pushed && clicks > 0) {
            wait = ticks_until(released_at, GTG_BUTTON_MULTI_CLICK_MS);
        }

        if (ulTaskNotifyTake(pdTRUE, wait) == 0) {
            if (pushed && !short_hold_posted) {
                /* pushed for the short hold time, the clicks before it are dropped */
                short_hold_posted = true;
                clicks = 0;
                post_button_event(GTG_EV_BUTTON_SHORT_HOLD);
            } else if (pushed && !long_hold_posted) {
                long_hold_posted = true;
                post_button_event(GTG_EV_BUTTON_LONG_HOLD);
            } else if (!pushed && clicks > 0) {
                /* no next push within the window, the sequence ended */
                post_clicks(clicks);
                clicks = 0;
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
            short_hold_posted = false;
            long_hold_posted = false;

            /* the window is checked here too, the push may come just after it but before the
             * task got to post the clicks */
            if (clicks > 0 && (pushed_at - released_at) / 1000 >= GTG_BUTTON_MULTI_CLICK_MS) {
                post_clicks(clicks);
                clicks = 0;
            }
            continue;
        }

        /* the release of a hold, which was posted while pushed */
        if (short_hold_posted) {
            continue;
        }

        int64_t now = esp_timer_get_time();
        ESP_LOGD(TAG, "released after %lld ms", (now - pushed_at) / 1000);

        clicks++;
        released_at = now;

        if (clicks == 3) {
            clicks = 0;
            post_button_event(GTG_EV_BUTTON_TRIPLE_CLICKED);
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
