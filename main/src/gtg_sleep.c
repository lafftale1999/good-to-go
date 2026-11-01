#include "gtg_sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_log.h"

#include "project_config.h"

#define RELEASE_POLL_MS     50

static const char *TAG = "sleep";

bool gtg_sleep_woke_by_button(void)
{
    /* a bit per esp_sleep_wakeup_cause_t, none after a power on or reset */
    return esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0);
}

void gtg_sleep_log_wakeup(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();

    if (gtg_sleep_woke_by_button()) {
        ESP_LOGI(TAG, "woke from deep sleep by the button");
    } else if (causes == 0) {
        ESP_LOGI(TAG, "started by power on or reset");
    } else {
        ESP_LOGI(TAG, "woke from deep sleep, causes 0x%08lx", (unsigned long)causes);
    }
}

void gtg_sleep_start(void)
{
    /* the wake is on the pushed level, so a held button would wake the device at once */
    if (gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL) {
        ESP_LOGI(TAG, "waiting for the button to be released before sleeping");
        while (gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL) {
            vTaskDelay(pdMS_TO_TICKS(RELEASE_POLL_MS));
        }
    }

    /* the digital pull-down is off in deep sleep, so the RTC one holds the pin low. ext0 keeps
     * the RTC peripherals powered, which the RTC pull-down needs. After the wake, gpio_config()
     * in the button driver gives the pin back to the digital GPIO */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, BUTTON_ACTIVE_LEVEL));
    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_en(BUTTON_GPIO));

    ESP_LOGI(TAG, "going to deep sleep, push the button to wake");
    esp_deep_sleep_start();
}
