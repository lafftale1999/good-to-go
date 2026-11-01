#ifndef GTG_LCD_H_
#define GTG_LCD_H_

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#define GTG_LCD_COLUMNS 16
#define GTG_LCD_ROWS    2

/**
 * The 16x2 LCD on the I2C bus set up in project_config.h, driven by the lcd_screen component.
 * All printing goes through this module, which keeps what is shown and writes it under a lock,
 * so several tasks can print without mixing up each other's lines.
 */
typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t       mutex;

    /* what the screen shows, a row per line */
    char                    lines[GTG_LCD_ROWS][GTG_LCD_COLUMNS + 1];
    bool                    initialized;
} gtg_lcd_module_t;

/**
 * \brief Opens the I2C bus, checks that the LCD answers and initializes it with the backlight on
 *
 * \param *lcd  the module to set up, zero-initialized, e.g. a static gtg_lcd_module_t
 *
 * \return ESP_OK                ready, the screen is cleared
 * \return ESP_ERR_INVALID_ARG   lcd is NULL
 * \return ESP_ERR_INVALID_STATE already initialized
 * \return ESP_ERR_NOT_FOUND     nothing answers at the LCD's address, check the wiring and address
 * \return ESP_ERR_NO_MEM        out of memory
 * \return other                 error from the I2C driver
 */
esp_err_t gtg_lcd_init(gtg_lcd_module_t *lcd);

/**
 * \brief Converts UTF-8 text to the characters the LCD can show
 *
 * The LCD has no å, ä or ö, so they become a, a and o, Å, Ä and Ö become A, A and O, and a few
 * other accented letters lose their accent. Any other non-ASCII character becomes '?'. Each
 * character takes one byte afterwards, so the length is what the screen shows.
 *
 * \param *dst       receives the text, cut to fit
 * \param dst_size   size of dst
 * \param *src       UTF-8 text, NULL for ""
 *
 * \return the length of dst
 */
size_t gtg_lcd_to_ascii(char *dst, size_t dst_size, const char *src);

/**
 * \brief Shows two lines, each cut to GTG_LCD_COLUMNS characters
 *
 * The lines are converted with gtg_lcd_to_ascii() first.
 *
 * Only a row that changed is written, so e.g. updating the times on the bottom row leaves the
 * top row untouched. Nothing is written when the screen already shows the same lines.
 *
 * \param *lcd     an initialized module
 * \param *top     first row, NULL or "" for an empty row
 * \param *bottom  second row, NULL or "" for an empty row
 *
 * \return ESP_OK                shown
 * \return ESP_ERR_INVALID_ARG   lcd is NULL
 * \return ESP_ERR_INVALID_STATE not initialized
 * \return ESP_FAIL              the LCD didn't take the text
 */
esp_err_t gtg_lcd_print(gtg_lcd_module_t *lcd, const char *top, const char *bottom);

/**
 * \brief Clears the screen
 */
esp_err_t gtg_lcd_clear(gtg_lcd_module_t *lcd);

/**
 * \brief Turns the backlight on or off, what the screen shows is kept
 *
 * It stays as set for later prints. The LCD is powered apart from the ESP32, so turn it off
 * before deep sleep. gtg_lcd_init() turns it on again.
 *
 * \return ESP_OK                set
 * \return ESP_ERR_INVALID_ARG   lcd is NULL
 * \return ESP_ERR_INVALID_STATE not initialized
 * \return ESP_FAIL              the LCD didn't take it
 */
esp_err_t gtg_lcd_set_backlight(gtg_lcd_module_t *lcd, bool on);

/**
 * \brief Releases the I2C bus and the resources taken by gtg_lcd_init()
 */
void gtg_lcd_deinit(gtg_lcd_module_t *lcd);

#endif
