#include "esp_err.h"
#include "gtg_app.h"

void app_main(void)
{
    gtg_app_handle_t app;

    ESP_ERROR_CHECK(gtg_app_init(&app));
    ESP_ERROR_CHECK(gtg_app_run(app));
}
