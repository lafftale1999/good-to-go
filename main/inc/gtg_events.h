#ifndef GTG_EVENTS_H_
#define GTG_EVENTS_H_

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#include "gtg_api_keys.h"

#define GTG_EVENTS_QUEUE_LEN    16

/**
 * Something that happened which the device's runtime behaviour depends on. Modules post events
 * when it happens, the device consumes them from the queue and decides what to do. The current
 * state, e.g. whether the device is connected, is kept in the gtg_dev_flag_t flags instead.
 */
typedef enum {
    GTG_EV_CONNECTION_CHANGED,  /*!< the station connected or lost its connection, see wifi */
    GTG_EV_API_KEY_REJECTED,    /*!< a service rejected a key, at verification or in use, see api_key */
    GTG_EV_BUTTON_CLICKED,      /*!< a button was released within the short hold time, see button */
    GTG_EV_BUTTON_SHORT_HOLD,   /*!< a button was released after the short hold time, see button */
    GTG_EV_BUTTON_LONG_HOLD,    /*!< a button has been held for the long hold time, still pushed, see button */
    GTG_EV_CONFIG_APPLIED,      /*!< GTG_DEV_FLAG_VALID_CONFIG got set, the device is fully configured */
} gtg_event_type_t;

typedef struct {
    gtg_event_type_t type;
    union {
        struct {
            bool    connected;
            uint8_t reason;     /*!< wifi_err_reason_t of a lost connection, 0 when connected */
        } wifi;                 /*!< GTG_EV_CONNECTION_CHANGED */

        struct {
            gtg_api_key_t which;
        } api_key;              /*!< GTG_EV_API_KEY_REJECTED */

        struct {
            uint8_t id;
        } button;               /*!< GTG_EV_BUTTON_CLICKED, _SHORT_HOLD and _LONG_HOLD */
    };
} gtg_event_t;

/**
 * \brief Creates the event queue, must be called before events are posted
 *
 * \return ESP_OK                ready
 * \return ESP_ERR_INVALID_STATE already initialized
 * \return ESP_ERR_NO_MEM        out of memory
 */
esp_err_t gtg_events_init(void);

/**
 * \brief Posts an event without blocking, from a task
 *
 * The poster is never held up, e.g. the WiFi event task. An event that doesn't fit in a full
 * queue is dropped and logged.
 *
 * \return ESP_OK                queued
 * \return ESP_ERR_INVALID_ARG   event is NULL
 * \return ESP_ERR_INVALID_STATE not initialized
 * \return ESP_ERR_NO_MEM        the queue is full, the event is dropped
 */
esp_err_t gtg_events_post(const gtg_event_t *event);

/**
 * \brief Posts an event from an interrupt, e.g. a button
 *
 * \param *event                 the event
 * \param *higher_priority_woken set to pdTRUE when a yield is needed before the ISR returns, can be NULL
 *
 * \return like gtg_events_post(), a full queue is not logged from the interrupt
 */
esp_err_t gtg_events_post_from_isr(const gtg_event_t *event, BaseType_t *higher_priority_woken);

/**
 * \brief Takes the next event from the queue
 *
 * \param *event  receives the event
 * \param ticks   maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                event received
 * \return ESP_ERR_INVALID_ARG   event is NULL
 * \return ESP_ERR_TIMEOUT       no event within the given time
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_events_receive(gtg_event_t *event, TickType_t ticks);

/**
 * \brief Name of an event type for logging
 */
const char *gtg_event_type_name(gtg_event_type_t type);

/**
 * \brief Posts GTG_EV_API_KEY_REJECTED, for the modules that use the keys
 */
void gtg_events_post_key_rejected(gtg_api_key_t which);

#endif
