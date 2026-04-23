/**
 ******************************************************************************
 * @file     main.c
 * @author   正点原子团队(ALIENTEK)
 * @version  V1.0
 * @date     2023-08-26
 * @brief    ESP32-IDF基础工程
 * @license  Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ******************************************************************************
 * @attention
 * 
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/gpio_struct.h"


#define TP_I2C_ADDR                 0x20
#define LRA_I2C_ADDR                0x5A

#define GPIO_SPI_MODE_SEL           GPIO_NUM_38
#define GPIO_SPI_MODE_SYNC          GPIO_NUM_39
#define GPIO_PANEL_RESET            GPIO_NUM_5
#define GPIO_PANEL_SWIRE            GPIO_NUM_1
#define GPIO_PANEL_VDDIO_1V8        GPIO_NUM_21
#define GPIO_PANEL_AVDD_2V8         GPIO_NUM_47
#define GPIO_PANEL_VCI_EN           GPIO_NUM_14

#define PANEL_BOOT_DELAY_MS         45
#define PANEL_RESET_SETUP_MS        5
#define PANEL_POWER_STABLE_DELAY_MS 5
#define PANEL_VCI_STABLE_DELAY_MS   2
#define PANEL_SWIRE_GAP_US          500
#define PANEL_SWIRE_LEVEL_US        10
#define PANEL_SWIRE_APPLY_DELAY_US  2000
#define PANEL_RESET_PULSE_US        100
#define PANEL_SWIRE_GPIO_MASK       (1UL << GPIO_PANEL_SWIRE)
#define PANEL_RESET_GPIO_MASK       (1UL << GPIO_PANEL_RESET)

static portMUX_TYPE panel_swire_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t panel_cpu_ticks_per_us;
static uint32_t panel_swire_level_cycles;
static uint32_t panel_swire_gap_cycles;
static uint32_t panel_swire_apply_cycles;
static uint32_t panel_reset_pulse_cycles;


static void panel_timing_init(void)
{
    panel_cpu_ticks_per_us = esp_rom_get_cpu_ticks_per_us();
    panel_swire_level_cycles = panel_cpu_ticks_per_us * PANEL_SWIRE_LEVEL_US;
    panel_swire_gap_cycles = panel_cpu_ticks_per_us * PANEL_SWIRE_GAP_US;
    panel_swire_apply_cycles = panel_cpu_ticks_per_us * PANEL_SWIRE_APPLY_DELAY_US;
    panel_reset_pulse_cycles = panel_cpu_ticks_per_us * PANEL_RESET_PULSE_US;
}


FORCE_INLINE_ATTR void panel_gpio_set_level_fast_low32(uint32_t gpio_mask, uint32_t level)
{
    if (level != 0)
    {
        GPIO.out_w1ts = gpio_mask;
    }
    else
    {
        GPIO.out_w1tc = gpio_mask;
    }
}

static void panel_delay_us(uint32_t delay_us)
{
    esp_rom_delay_us(delay_us);
}


static void panel_delay_ms_busy(uint32_t delay_ms)
{
    if (delay_ms == 0)
    {
        return;
    }

    esp_rom_delay_us(delay_ms * 1000);
}


static IRAM_ATTR void panel_delay_cycles(uint32_t delay_cycles)
{
    if (delay_cycles == 0)
    {
        return;
    }

    const esp_cpu_cycle_count_t start_cycle = esp_cpu_get_cycle_count();

    while ((esp_cpu_cycle_count_t)(esp_cpu_get_cycle_count() - start_cycle) < delay_cycles)
    {
    }
}


static IRAM_ATTR void panel_swire_sync_pulse_nolock(void)
{
    panel_gpio_set_level_fast_low32(PANEL_SWIRE_GPIO_MASK, 0);
    panel_delay_cycles(panel_swire_level_cycles);
    panel_gpio_set_level_fast_low32(PANEL_SWIRE_GPIO_MASK, 1);
    panel_delay_cycles(panel_swire_level_cycles);
}


static IRAM_ATTR void panel_swire_send_pulses_nolock(uint32_t pulse_count)
{
    for (uint32_t pulse_index = 0; pulse_index < pulse_count; pulse_index++)
    {
        panel_swire_sync_pulse_nolock();
    }
}


static IRAM_ATTR void panel_swire_send_setting_nolock(uint32_t pulse_count, uint32_t hold_cycles)
{
    panel_swire_send_pulses_nolock(pulse_count);
    panel_gpio_set_level_fast_low32(PANEL_SWIRE_GPIO_MASK, 1);
    panel_delay_cycles(hold_cycles);
}


static void panel_gpio_config_output(gpio_num_t gpio_num, uint32_t initial_level)
{
    ESP_ERROR_CHECK(gpio_hold_dis(gpio_num));
    ESP_ERROR_CHECK(gpio_set_level(gpio_num, initial_level));

    const gpio_config_t gpio_output_config = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&gpio_output_config));
}


static void panel_power_on_init(void)
{
    panel_timing_init();

    panel_gpio_config_output(GPIO_SPI_MODE_SEL, 1);
    panel_gpio_config_output(GPIO_SPI_MODE_SYNC, 0);
    panel_gpio_config_output(GPIO_PANEL_RESET, 0);
    panel_gpio_config_output(GPIO_PANEL_SWIRE, 1);
    panel_gpio_config_output(GPIO_PANEL_VDDIO_1V8, 0);
    panel_gpio_config_output(GPIO_PANEL_AVDD_2V8, 0);
    panel_gpio_config_output(GPIO_PANEL_VCI_EN, 0);

    panel_delay_ms_busy(PANEL_BOOT_DELAY_MS);

    gpio_set_level(GPIO_PANEL_RESET, 1);
    panel_delay_ms_busy(PANEL_RESET_SETUP_MS);

    gpio_set_level(GPIO_PANEL_VDDIO_1V8, 1);
    gpio_set_level(GPIO_PANEL_AVDD_2V8, 1);
    panel_delay_ms_busy(PANEL_POWER_STABLE_DELAY_MS);

    gpio_set_level(GPIO_PANEL_VCI_EN, 1);
    panel_delay_ms_busy(PANEL_VCI_STABLE_DELAY_MS);

    taskENTER_CRITICAL(&panel_swire_mux);
    
    panel_swire_send_setting_nolock(1, panel_swire_gap_cycles);
    panel_delay_ms_busy(1);
    panel_swire_send_setting_nolock(17, panel_swire_gap_cycles);

    panel_swire_send_setting_nolock(1, panel_swire_gap_cycles);
    panel_delay_ms_busy(1);
    panel_swire_send_setting_nolock(63, panel_swire_gap_cycles);

    panel_swire_send_setting_nolock(1, panel_swire_gap_cycles);
    panel_delay_ms_busy(1);
    panel_swire_send_setting_nolock(116, panel_swire_gap_cycles);

    panel_swire_send_setting_nolock(1, panel_swire_gap_cycles);
    panel_delay_ms_busy(1);
    panel_swire_send_setting_nolock(117, panel_swire_apply_cycles);

    panel_gpio_set_level_fast_low32(PANEL_SWIRE_GPIO_MASK, 1);
    panel_gpio_set_level_fast_low32(PANEL_RESET_GPIO_MASK, 0);
    panel_delay_cycles(panel_reset_pulse_cycles);
    panel_gpio_set_level_fast_low32(PANEL_RESET_GPIO_MASK, 1);

    taskEXIT_CRITICAL(&panel_swire_mux);

    printf("TP IC Addr: 0x%02X, LRA Addr: 0x%02X\n", TP_I2C_ADDR, LRA_I2C_ADDR);
}


/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    uint32_t flash_size;
    esp_chip_info_t chip_info;                                      /* 定义芯片信息结构体变量 */

    panel_power_on_init();

    ret = nvs_flash_init();                                         /* 初始化NVS */

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    esp_flash_get_size(NULL, &flash_size);                          /* 获取FLASH大小 */

    esp_chip_info(&chip_info);
    printf("内核：cup数量%d\n",chip_info.cores);                     /* 获取CPU内核数并显示 */
    printf("FLASH size:%ld MB flash\n",flash_size / (1024 * 1024)); /* 获取FLASH大小并显示 */
    printf("PSRAM size: %d bytes\n", esp_psram_get_size());         /* 获取PARAM大小并显示 */

    while(1)
    {
        printf("Hello-ESP32\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
