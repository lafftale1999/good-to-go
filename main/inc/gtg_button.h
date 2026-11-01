#ifndef GTG_BUTTON_H_
#define GTG_BUTTON_H_

#include "esp_err.h"

#define GTG_BUTTON_ID_MAIN          0       /*!< button.id of the button on BUTTON_GPIO */

#define GTG_BUTTON_DEBOUNCE_MS      30      /*!< a level must stay this long to count */
#define GTG_BUTTON_SHORT_HOLD_MS    5000    /*!< released after this, a short hold */
#define GTG_BUTTON_LONG_HOLD_MS     20000   /*!< held this long, a long hold */
#define GTG_BUTTON_DOUBLE_CLICK_MS  300     /*!< from a click's release to the next push, a double-click */

/**
 * \brief Starts reading the button on BUTTON_GPIO from project_config.h
 *
 * The button connects the GPIO to 3V3 and is read with the internal pull-down. An interrupt on
 * each edge wakes a task that debounces the button and times each push. Each push posts one
 * event to the event queue, see gtg_events.h:
 *  - released before GTG_BUTTON_SHORT_HOLD_MS:  GTG_EV_BUTTON_CLICKED, GTG_BUTTON_DOUBLE_CLICK_MS
 *                                               after the release when no second push follows
 *  - two clicks, the second pushed within GTG_BUTTON_DOUBLE_CLICK_MS of the first's release:
 *                                               GTG_EV_BUTTON_DOUBLE_CLICKED, on the second release,
 *                                               instead of the clicks
 *  - released before GTG_BUTTON_LONG_HOLD_MS:   GTG_EV_BUTTON_SHORT_HOLD, on release
 *  - held for GTG_BUTTON_LONG_HOLD_MS:          GTG_EV_BUTTON_LONG_HOLD, right away while still
 *                                               pushed, the release after it posts nothing
 *
 * A hold that is the second push of a double-click drops the first click, only the hold is posted.
 *
 * A push that is already held when this is called, e.g. the one that woke the device from deep
 * sleep, posts nothing.
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
