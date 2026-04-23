#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "spi.h"
#include "lcd.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "SPI2 init");
    spi2_init();

    ESP_LOGI(TAG, "LCD init");
    lcd_init();

    ESP_LOGI(TAG, "LCD clear to white");
    lcd_clear(WHITE);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
