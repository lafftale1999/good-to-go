#ifndef GTG_BUTTON_H_
#define GTG_BUTTON_H_

#include "esp_err.h"

#define GTG_BUTTON_ID_MAIN          0       /*!< button.id of the button on BUTTON_GPIO */

#define GTG_BUTTON_DEBOUNCE_MS      30      /*!< a level must stay this long to count */
#define GTG_BUTTON_SHORT_HOLD_MS    5000    /*!< held this long, a short hold */
#define GTG_BUTTON_LONG_HOLD_MS     20000   /*!< held this long, a long hold */
#define GTG_BUTTON_MULTI_CLICK_MS   300     /*!< from a click's release to the next push, the same sequence */

/**
 * \brief Starts reading the button on BUTTON_GPIO from project_config.h
 *
 * The button connects the GPIO to 3V3 and is read with the internal pull-down. An interrupt on
 * each edge wakes a task that debounces the button and times each push. It posts these events to
 * the event queue, see gtg_events.h:
 *  - GTG_EV_BUTTON_CLICKED:         released before GTG_BUTTON_SHORT_HOLD_MS, posted
 *                                   GTG_BUTTON_MULTI_CLICK_MS after the release when no next push
 *                                   follows. Two clicks in a row post two of them
 *  - GTG_EV_BUTTON_TRIPLE_CLICKED:  three clicks, each pushed within GTG_BUTTON_MULTI_CLICK_MS of
 *                                   the last one's release, posted on the third release instead of
 *                                   the clicks
 *  - GTG_EV_BUTTON_SHORT_HOLD:      held for GTG_BUTTON_SHORT_HOLD_MS, right away while still pushed
 *  - GTG_EV_BUTTON_LONG_HOLD:       still held at GTG_BUTTON_LONG_HOLD_MS, right away while still
 *                                   pushed. The short hold was posted before it
 *
 * The release after a hold posts nothing. A hold that is part of a click sequence drops the clicks
 * before it, only the hold is posted.
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
