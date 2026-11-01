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

/*
 * Each edge wakes the task, which waits out the bounce and then reads the level. Edges during
 * the wait are dropped before reading, so a bounce only counts as one change, while an edge
 * after the read wakes the task again. A push is timed from its debounced start.
 *
 * While the button is pushed the task also wakes at the long hold time, and posts the long hold
 * right then without waiting for the release. The release after a long hold posts nothing.
 *
 * A click isn't posted on its release: the task waits GTG_BUTTON_DOUBLE_CLICK_MS for a second
 * push first. Without one the click is posted, with one the first click waits for how the second
 * push ends. Released as a click the two are one double-click, held to a short or long hold the
 * first click is dropped and only the hold is posted.
 */
static void button_task(void *arg)
{
    bool pushed = is_pushed();
    bool long_hold_posted = false;
    int64_t pushed_at = esp_timer_get_time();

    /* a click released, waiting for a second push within the double-click window */
    bool click_pending = false;
    int64_t released_at = 0;

    /* the push follows a click within the window, so it ends in a double-click or a hold */
    bool second_push = false;

    if (pushed) {
        /* e.g. the push that woke the device from deep sleep, which isn't a click. Marked as
         * handled, so neither its release nor a long hold posts anything */
        ESP_LOGI(TAG, "pushed at start, ignoring this push");
        long_hold_posted = true;
    }

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (pushed && !long_hold_posted) {
            wait = ticks_until(pushed_at, GTG_BUTTON_LONG_HOLD_MS);
        } else if (click_pending) {
            wait = ticks_until(released_at, GTG_BUTTON_DOUBLE_CLICK_MS);
        }

        if (ulTaskNotifyTake(pdTRUE, wait) == 0) {
            if (pushed && !long_hold_posted) {
                /* pushed for the long hold time, a first click before it is dropped */
                long_hold_posted = true;
                second_push = false;
                post_button_event(GTG_EV_BUTTON_LONG_HOLD);
            } else if (click_pending) {
                /* no second push within the window */
                click_pending = false;
                post_button_event(GTG_EV_BUTTON_CLICKED);
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

            /* the window is checked here too, the press may come just after it but before
             * the task got to post the click */
            second_push = click_pending
                && (pushed_at - released_at) / 1000 < GTG_BUTTON_DOUBLE_CLICK_MS;
            if (click_pending && !second_push) {
                post_button_event(GTG_EV_BUTTON_CLICKED);
            }
            click_pending = false;
            continue;
        }

        if (long_hold_posted) {
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t held_ms = (now - pushed_at) / 1000;
        ESP_LOGD(TAG, "released after %lld ms", held_ms);

        if (held_ms >= GTG_BUTTON_SHORT_HOLD_MS) {
            /* a first click before it is dropped */
            post_button_event(GTG_EV_BUTTON_SHORT_HOLD);
        } else if (second_push) {
            post_button_event(GTG_EV_BUTTON_DOUBLE_CLICKED);
        } else {
            click_pending = true;
            released_at = now;
        }
        second_push = false;
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
