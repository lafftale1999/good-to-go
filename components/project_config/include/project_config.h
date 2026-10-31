#ifndef PROJECT_CONFIG_H_
#define PROJECT_CONFIG_H_

/* Hardware configuration of the project, read by components such as the LCD driver in
 * components/lcd_screen */

#include "soc/gpio_num.h"
#include "driver/i2c_types.h"

// SET UP FOR I2C COMMUNICATION. USED FOR THE LCD 16X2 SCREEN.
#define I2C_MASTER_SCL_IO           GPIO_NUM_22                 /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           GPIO_NUM_21                 /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              I2C_NUM_0                   /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ          100000                      /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000
#define I2C_DEVICE_ADDRESS_LEN      I2C_ADDR_BIT_LEN_7

#endif
