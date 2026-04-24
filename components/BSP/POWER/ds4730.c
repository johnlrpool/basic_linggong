#include "ds4730.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PANEL_BOOT_DELAY_MS          45
#define PANEL_RESET_SETUP_MS         5
#define PANEL_POWER_STABLE_DELAY_MS  5
#define PANEL_RT4730_ENABLE_DELAY_MS 2
#define PANEL_PRECODE_DELAY_MS       1
#define PANEL_POST_RESET_DELAY_MS    10
#define PANEL_SWIRE_LEVEL_US         10
#define PANEL_SWIRE_GAP_US           500
#define PANEL_SWIRE_APPLY_DELAY_US   2000
#define PANEL_RESET_PULSE_US         100

#define PANEL_RST(x)       do { gpio_set_level(PANEL_NUM_RST, (x) ? 1 : 0); } while (0)
#define PANEL_IM1(x)       do { gpio_set_level(PANEL_NUM_IM1, (x) ? 1 : 0); } while (0)
#define PANEL_IM0(x)       do { gpio_set_level(PANEL_NUM_IM0, (x) ? 1 : 0); } while (0)
#define PANEL_SWIRE(x)     do { gpio_set_level(PANEL_NUM_SWIRE, (x) ? 1 : 0); } while (0)
#define PANEL_VDDIO(x)     do { gpio_set_level(PANEL_NUM_VDDIO_1V8, (x) ? 1 : 0); } while (0)
#define PANEL_AVDD(x)      do { gpio_set_level(PANEL_NUM_AVDD_2V8, (x) ? 1 : 0); } while (0)
#define PANEL_RT4730_EN(x) do { gpio_set_level(PANEL_NUM_RT4730_EN, (x) ? 1 : 0); } while (0)

static const char *TAG = "ds4730";
static bool panel_powered_on;
static portMUX_TYPE panel_swire_mux = portMUX_INITIALIZER_UNLOCKED;

static void panel_delay_us(uint32_t delay_us)
{
    if (delay_us == 0) {
        return;
    }
    esp_rom_delay_us(delay_us);
}

static void panel_delay_ms_busy(uint32_t delay_ms)
{
    if (delay_ms == 0) {
        return;
    }
    esp_rom_delay_us(delay_ms * 1000);
}

static esp_err_t panel_gpio_config_output(gpio_num_t gpio_num, uint32_t initial_level)
{
    const gpio_config_t gpio_output_config = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_hold_dis(gpio_num), TAG, "gpio %d hold disable failed", gpio_num);
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_output_config), TAG, "gpio %d config failed", gpio_num);
    return gpio_set_level(gpio_num, initial_level);
}

static void panel_swire_sync_pulse_nolock(void)
{
    PANEL_SWIRE(0);
    panel_delay_us(PANEL_SWIRE_LEVEL_US);
    PANEL_SWIRE(1);
    panel_delay_us(PANEL_SWIRE_LEVEL_US);
}

static void panel_swire_send_pulses_nolock(uint32_t pulse_count)
{
    for (uint32_t pulse_index = 0; pulse_index < pulse_count; pulse_index++) {
        panel_swire_sync_pulse_nolock();
    }
}

static void panel_swire_send_setting_nolock(uint32_t pulse_count, uint32_t hold_us)
{
    panel_swire_send_pulses_nolock(pulse_count);
    PANEL_SWIRE(1);
    panel_delay_us(hold_us);
}

static void panel_config_power_sequence_gpios(void)
{
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_IM1, 1));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_IM0, 0));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_RST, 0));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_SWIRE, 1));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_VDDIO_1V8, 0));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_AVDD_2V8, 0));
    ESP_ERROR_CHECK(panel_gpio_config_output(PANEL_NUM_RT4730_EN, 0));
}

static void panel_enable_power_rails(void)
{
    PANEL_VDDIO(1);
    PANEL_AVDD(1);
    panel_delay_ms_busy(PANEL_POWER_STABLE_DELAY_MS);

    PANEL_RT4730_EN(1);
    panel_delay_ms_busy(PANEL_RT4730_ENABLE_DELAY_MS);
}

static void panel_program_rt4730_swire(void)
{
    /*
     * The schematic notes match the legacy bring-up code: each RT4730 target
     * value is preceded by a single SWIRE sync pulse, then a 1 ms spacer.
     */
    panel_swire_send_setting_nolock(1, PANEL_SWIRE_GAP_US);
    panel_delay_ms_busy(PANEL_PRECODE_DELAY_MS);
    panel_swire_send_setting_nolock(17, PANEL_SWIRE_GAP_US);

    panel_swire_send_setting_nolock(1, PANEL_SWIRE_GAP_US);
    panel_delay_ms_busy(PANEL_PRECODE_DELAY_MS);
    panel_swire_send_setting_nolock(63, PANEL_SWIRE_GAP_US);

    panel_swire_send_setting_nolock(1, PANEL_SWIRE_GAP_US);
    panel_delay_ms_busy(PANEL_PRECODE_DELAY_MS);
    panel_swire_send_setting_nolock(116, PANEL_SWIRE_GAP_US);

    panel_swire_send_setting_nolock(1, PANEL_SWIRE_GAP_US);
    panel_delay_ms_busy(PANEL_PRECODE_DELAY_MS);
    panel_swire_send_setting_nolock(117, PANEL_SWIRE_APPLY_DELAY_US);
}

static void panel_hard_reset_pulse_nolock(void)
{
    PANEL_RST(0);
    panel_delay_us(PANEL_RESET_PULSE_US);
    PANEL_RST(1);
}

void ds4730_panel_hard_reset(void)
{
    ESP_LOGI(TAG, "panel hard reset pulse");
    panel_hard_reset_pulse_nolock();
    vTaskDelay(pdMS_TO_TICKS(PANEL_POST_RESET_DELAY_MS));
}

void ds4730_panel_power_on_sequence(void)
{
    if (panel_powered_on) {
        ESP_LOGI(TAG, "panel power sequence already completed");
        return;
    }

    ESP_LOGI(TAG, "power-up start: IM1=%d IM0=%d RST=%d SWIRE=%d VDDIO=%d AVDD=%d RT4730_EN=%d",
             PANEL_NUM_IM1, PANEL_NUM_IM0, PANEL_NUM_RST, PANEL_NUM_SWIRE,
             PANEL_NUM_VDDIO_1V8, PANEL_NUM_AVDD_2V8, PANEL_NUM_RT4730_EN);

    panel_config_power_sequence_gpios();

    panel_delay_ms_busy(PANEL_BOOT_DELAY_MS);

    PANEL_RST(1);
    panel_delay_ms_busy(PANEL_RESET_SETUP_MS);

    ESP_LOGI(TAG, "enabling panel rails: VDDIO 1.8V + AVDD 2.8V");
    panel_enable_power_rails();

    taskENTER_CRITICAL(&panel_swire_mux);

    ESP_LOGI(TAG, "programming RT4730 SWIRE levels: 17 -> 63 -> 116 -> 117");
    panel_program_rt4730_swire();
    PANEL_SWIRE(1);
    panel_hard_reset_pulse_nolock();

    taskEXIT_CRITICAL(&panel_swire_mux);

    vTaskDelay(pdMS_TO_TICKS(PANEL_POST_RESET_DELAY_MS));
    panel_powered_on = true;
    ESP_LOGI(TAG, "power-up complete");
}
