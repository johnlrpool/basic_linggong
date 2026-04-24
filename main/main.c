#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "icna3312.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "starting ICNA3312 white-screen bring-up");
    lcd_init();
    lcd_run_white_smoke_test();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
