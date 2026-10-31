#ifndef GTG_BUTTON_H_
#define GTG_BUTTON_H_

#include "esp_err.h"

#define GTG_BUTTON_ID_MAIN          0       /*!< button.id of the button on BUTTON_GPIO */

#define GTG_BUTTON_DEBOUNCE_MS      30      /*!< a level must stay this long to count */
#define GTG_BUTTON_SHORT_HOLD_MS    5000    /*!< released after this, a short hold */
#define GTG_BUTTON_LONG_HOLD_MS     20000   /*!< released after this, a long hold */

/**
 * \brief Starts reading the button on BUTTON_GPIO from project_config.h
 *
 * The button connects the GPIO to 3V3 and is read with the internal pull-down. An interrupt on
 * each edge wakes a task that debounces the button and times each push. Each push posts one
 * event to the event queue, see gtg_events.h:
 *  - released before GTG_BUTTON_SHORT_HOLD_MS:  GTG_EV_BUTTON_CLICKED, on release
 *  - released before GTG_BUTTON_LONG_HOLD_MS:   GTG_EV_BUTTON_SHORT_HOLD, on release
 *  - held for GTG_BUTTON_LONG_HOLD_MS:          GTG_EV_BUTTON_LONG_HOLD, right away while still
 *                                               pushed, the release after it posts nothing
 *
 * gtg_events_init() must have been called.
 *
 * \return ESP_OK                started
 * \return ESP_ERR_INVALID_STATE already started
 * \return ESP_ERR_NO_MEM        out of memory
 * \return other                 error from the GPIO driver
 */
esp_err_t gtg_button_init(void);

#endif
