#ifndef GTG_SLEEP_H_
#define GTG_SLEEP_H_

#include <stdbool.h>

/**
 * Deep sleep, woken by the button on BUTTON_GPIO from project_config.h. Waking restarts the
 * device, so it boots as after a power on and gtg_sleep_woke_by_button() tells them apart.
 */

/**
 * \brief Logs why the device started, call once early at boot
 */
void gtg_sleep_log_wakeup(void);

/**
 * \brief Whether this boot is a wake from deep sleep by the button
 */
bool gtg_sleep_woke_by_button(void);

/**
 * \brief Goes into deep sleep until the button is pushed, doesn't return
 *
 * Waits for the button to be released first, a pushed button would wake the device right away.
 * The pin keeps its pull-down through the sleep. The caller shows its own message and turns off
 * the LCD's backlight before.
 */
void gtg_sleep_start(void) __attribute__((noreturn));

#endif
