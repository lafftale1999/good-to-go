#include "gtg_lcd.h"

#include <string.h>
#include "esp_log.h"

#include "lcd_1602.h"

static const char *TAG = "lcd";

esp_err_t gtg_lcd_init(gtg_lcd_module_t *lcd)
{
    if (lcd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lcd->initialized) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(lcd, 0, sizeof(*lcd));

    lcd->mutex = xSemaphoreCreateMutex();
    if (lcd->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* the driver sets up the bus and the device with the pins from project_config.h */
    i2c_open(&lcd->bus, &lcd->dev, DEVICE_ADDRESS);

    /* the driver doesn't report a missing screen, so check that something answers first */
    esp_err_t err = i2c_master_probe(lcd->bus, DEVICE_ADDRESS, I2C_MASTER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no LCD answers at 0x%02x on SDA %d / SCL %d: %s",
                 DEVICE_ADDRESS, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, esp_err_to_name(err));
        gtg_lcd_deinit(lcd);
        return err;
    }

    if (lcd_1602_init(lcd->dev) != 0) {
        ESP_LOGE(TAG, "LCD at 0x%02x didn't take the start sequence", DEVICE_ADDRESS);
        gtg_lcd_deinit(lcd);
        return ESP_FAIL;
    }
    lcd->initialized = true;

    ESP_LOGI(TAG, "LCD ready at 0x%02x", DEVICE_ADDRESS);

    return ESP_OK;
}

/* Copies a line, cut to the width of the screen */
static void copy_line(char *dst, const char *src)
{
    strlcpy(dst, src != NULL ? src : "", GTG_LCD_COLUMNS + 1);
}

esp_err_t gtg_lcd_print(gtg_lcd_module_t *lcd, const char *top, const char *bottom)
{
    if (lcd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lcd->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    char lines[GTG_LCD_ROWS][GTG_LCD_COLUMNS + 1];
    copy_line(lines[0], top);
    copy_line(lines[1], bottom);

    /* the driver writes one row at a time and pads it with blanks, so only a changed row is
     * written and the other one stays as it is, without clearing the screen */
    LCD_WRITE_STATUS (*const write_row[GTG_LCD_ROWS])(i2c_master_dev_handle_t, const char *) = {
        lcd_1602_write_top,
        lcd_1602_write_bottom,
    };

    xSemaphoreTake(lcd->mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    for (int row = 0; row < GTG_LCD_ROWS; row++) {
        if (strcmp(lines[row], lcd->lines[row]) == 0) {
            continue;
        }

        /* the lines are already cut to the row, so a too long string can't happen */
        if (write_row[row](lcd->dev, lines[row]) == LCD_WRITE_INTERRUPTED) {
            /* unknown what the row shows now, so the next print writes it again */
            memset(lcd->lines[row], 0xff, GTG_LCD_COLUMNS);
            lcd->lines[row][GTG_LCD_COLUMNS] = '\0';
            err = ESP_FAIL;
        } else {
            memcpy(lcd->lines[row], lines[row], sizeof(lines[row]));
        }
    }

    xSemaphoreGive(lcd->mutex);

    return err;
}

esp_err_t gtg_lcd_clear(gtg_lcd_module_t *lcd)
{
    if (lcd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lcd->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(lcd->mutex, portMAX_DELAY);
    uint8_t err = lcd_1602_clear_screen(lcd->dev);
    memset(lcd->lines, 0, sizeof(lcd->lines));
    xSemaphoreGive(lcd->mutex);

    return err == 0 ? ESP_OK : ESP_FAIL;
}

void gtg_lcd_deinit(gtg_lcd_module_t *lcd)
{
    if (lcd == NULL) {
        return;
    }

    if (lcd->dev != NULL) {
        i2c_master_bus_rm_device(lcd->dev);
    }
    if (lcd->bus != NULL) {
        i2c_del_master_bus(lcd->bus);
    }
    if (lcd->mutex != NULL) {
        vSemaphoreDelete(lcd->mutex);
    }

    memset(lcd, 0, sizeof(*lcd));
}
