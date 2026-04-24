/**
 ****************************************************************************************************
 * @file        spi.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2023-08-26
 * @brief       SPI驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#ifndef __SPI_H
#define __SPI_H

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"


/* 引脚定义：根据用户接线修改 */
#define SPI_MOSI_GPIO_PIN   GPIO_NUM_7          /* SPI2_MOSI */
#define SPI_CLK_GPIO_PIN    GPIO_NUM_16         /* SPI2_CLK */
#define SPI_MISO_GPIO_PIN   GPIO_NUM_6          /* SPI2_MISO / LCD SDO */

/* 函数声明 */
void spi2_init(void);                                                               /* 初始化SPI2 */
void spi2_write_cmd(spi_device_handle_t handle, uint8_t cmd);                       /* SPI发送命令 */
void spi2_write_data(spi_device_handle_t handle, const uint8_t *data, int len);     /* SPI发送数据 */
esp_err_t spi2_add_device(const spi_device_interface_config_t *devcfg,
                          spi_device_handle_t *handle);                              /* SPI添加设备 */
esp_err_t spi2_transmit(spi_device_handle_t handle, spi_transaction_t *trans);       /* SPI发送事务 */

#endif
