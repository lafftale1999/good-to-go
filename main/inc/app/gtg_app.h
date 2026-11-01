#ifndef GTG_APPLICATION_H_
#define GTG_APPLICATION_H_

#include "esp_err.h"

/**
 * Handle to the application. The struct is only defined in gtg_app.c, so callers can hold and
 * pass the handle but can't reach its members, everything goes through the functions below.
 */
typedef struct gtg_app *gtg_app_handle_t;

/**
 * \brief Creates the application and runs the startup steps
 *
 * \param *out_app  receives the handle on success, untouched on failure
 *
 * \return ESP_OK                ready
 * \return ESP_ERR_INVALID_ARG   out_app is NULL
 * \return ESP_ERR_NO_MEM        out of memory
 */
esp_err_t gtg_app_init(gtg_app_handle_t *out_app);

esp_err_t gtg_app_run(gtg_app_handle_t app);

#endif
