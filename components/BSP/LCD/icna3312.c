#include "icna3312.h"

#include <stdbool.h>

#include "ds4730.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "spi.h"

#define LCD_QSPI_WRITE_OPCODE             0x02
#define LCD_QSPI_READ_OPCODE              0x03
#define LCD_QSPI_MODE_OPCODE              0x38
#define LCD_SPI_CLOCK_HZ                  (10 * 1000 * 1000)
#define LCD_RESET_TO_FIRST_CMD_DELAY_MS   120
#define LCD_QSPI_MODE_DELAY_MS            1
#define LCD_SLPOUT_DELAY_MS               120
#define LCD_DISPON_DELAY_MS               20

static const char *TAG = "icna3312";

spi_device_handle_t MY_LCD_Handle;
uint8_t lcd_buf[LCD_BUF_SIZE];
lcd_obj_t lcd_self;

static bool lcd_bus_ready;
static bool lcd_initialized;
static bool lcd_pending_cmd_valid;
static bool lcd_memory_write_active;
static bool lcd_qspi_mode_enabled;
static bool lcd_id_valid;
static uint8_t lcd_pending_cmd;
static uint8_t lcd_id_cache[3];

static void lcd_delay_ms(uint32_t delay_ms)
{
    if (delay_ms == 0) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static esp_err_t lcd_gpio_config_output(gpio_num_t gpio_num, uint32_t initial_level)
{
    if (gpio_num < 0) {
        return ESP_OK;
    }

    const gpio_config_t gpio_output_config = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&gpio_output_config), TAG, "gpio %d config failed", gpio_num);
    return gpio_set_level(gpio_num, initial_level);
}

static esp_err_t lcd_gpio_config_input(gpio_num_t gpio_num)
{
    if (gpio_num < 0) {
        return ESP_OK;
    }

    const gpio_config_t gpio_input_config = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&gpio_input_config);
}

static void lcd_panel_sideband_gpio_init(void)
{
    ESP_ERROR_CHECK(lcd_gpio_config_output(LCD_NUM_SIO1, 0));
    ESP_ERROR_CHECK(lcd_gpio_config_output(LCD_NUM_SIO2, 0));
    ESP_ERROR_CHECK(lcd_gpio_config_output(LCD_NUM_SIO3, 0));
    ESP_ERROR_CHECK(lcd_gpio_config_input(LCD_NUM_TE));
    ESP_ERROR_CHECK(lcd_gpio_config_input(LCD_NUM_UNUSED_IO2));
    ESP_ERROR_CHECK(lcd_gpio_config_input(LCD_NUM_UNUSED_IO3));
}

static bool lcd_scan_dir_swaps_axes(uint8_t dir)
{
    return (dir == U2D_L2R) || (dir == U2D_R2L) ||
           (dir == D2U_L2R) || (dir == D2U_R2L);
}

static void lcd_update_display_state(bool swap_axes)
{
    lcd_self.dir = swap_axes ? 1 : 0;
    lcd_self.width = swap_axes ? ICNA3312_LCD_H : ICNA3312_LCD_W;
    lcd_self.height = swap_axes ? ICNA3312_LCD_W : ICNA3312_LCD_H;
    lcd_self.wramcmd = ICNA3312_RAMWR;
    lcd_self.setxcmd = ICNA3312_CASET;
    lcd_self.setycmd = ICNA3312_RASET;
    lcd_self.cs = LCD_NUM_CS;
}

static bool lcd_cmd_accepts_payload(uint8_t cmd)
{
    switch (cmd) {
        case ICNA3312_CASET:
        case ICNA3312_RASET:
        case ICNA3312_RAMWR:
        case ICNA3312_RAMWRC:
        case ICNA3312_TEON:
        case ICNA3312_MADCTL:
        case ICNA3312_COLMOD:
        case ICNA3312_WRDISBV:
        case ICNA3312_WRCTRLD:
        case ICNA3312_WRACL:
        case ICNA3312_SETDISPMODE:
        case ICNA3312_SETDSPIMODE:
        case ICNA3312_PAGESEL:
            return true;
        default:
            return false;
    }
}

static bool lcd_is_memory_write_cmd(uint8_t cmd)
{
    return (cmd == ICNA3312_RAMWR) || (cmd == ICNA3312_RAMWRC);
}

static esp_err_t lcd_qspi_enable_mode(void)
{
    if (lcd_qspi_mode_enabled) {
        return ESP_OK;
    }

    uint8_t opcode = LCD_QSPI_MODE_OPCODE;
    spi_transaction_ext_t transaction = {0};

    transaction.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    transaction.base.length = 8;
    transaction.base.tx_buffer = &opcode;
    transaction.command_bits = 0;
    transaction.address_bits = 0;
    transaction.dummy_bits = 0;

    ESP_RETURN_ON_ERROR(spi2_transmit(MY_LCD_Handle, (spi_transaction_t *)&transaction),
                        TAG, "send qspi mode opcode failed");
    lcd_delay_ms(LCD_QSPI_MODE_DELAY_MS);

    lcd_qspi_mode_enabled = true;
    ESP_LOGI(TAG, "entered panel serial mode via SPI opcode 0x%02X", LCD_QSPI_MODE_OPCODE);
    return ESP_OK;
}

static esp_err_t lcd_panel_transfer(uint8_t opcode,
                                    uint8_t reg,
                                    const uint8_t *tx_data,
                                    size_t tx_len,
                                    uint8_t *rx_data,
                                    size_t rx_len)
{
    spi_transaction_ext_t transaction = {0};

    if (!lcd_bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(lcd_qspi_enable_mode(), TAG, "panel transport not ready");

    transaction.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    transaction.base.cmd = opcode;
    /* Preserve the legacy wire header: [opcode][0x00][reg][0x00]. */
    transaction.base.addr = ((uint32_t)reg) << 8;
    transaction.base.length = tx_len * 8U;
    transaction.base.rxlength = rx_len * 8U;
    transaction.base.tx_buffer = tx_data;
    transaction.base.rx_buffer = rx_data;
    transaction.command_bits = 8;
    transaction.address_bits = 24;
    transaction.dummy_bits = 0;

    return spi2_transmit(MY_LCD_Handle, (spi_transaction_t *)&transaction);
}

static esp_err_t lcd_panel_write(uint8_t reg, const uint8_t *data, size_t len)
{
    return lcd_panel_transfer(LCD_QSPI_WRITE_OPCODE, reg, data, len, NULL, 0);
}

static esp_err_t lcd_panel_read(uint8_t reg, uint8_t *data, size_t len)
{
    return lcd_panel_transfer(LCD_QSPI_READ_OPCODE, reg, NULL, 0, data, len);
}

static void lcd_flush_pending_cmd(void)
{
    esp_err_t err;

    if (!lcd_pending_cmd_valid) {
        return;
    }

    err = lcd_panel_write(lcd_pending_cmd, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write cmd 0x%02X failed: %s", lcd_pending_cmd, esp_err_to_name(err));
    }

    lcd_memory_write_active = lcd_is_memory_write_cmd(lcd_pending_cmd);
    lcd_pending_cmd_valid = false;
}

static void lcd_send_cmd_params(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_write_cmd(cmd);

    if ((data == NULL) || (len == 0)) {
        lcd_flush_pending_cmd();
        return;
    }

    lcd_write_data(data, (int)len);
}

static void lcd_finish_memory_write(void)
{
    lcd_memory_write_active = false;
}

static bool lcd_id_looks_valid(const uint8_t id[3])
{
    if (id == NULL) {
        return false;
    }

    return (id[0] == 0x33) && (id[2] == 0x00) &&
           ((id[1] == 0x10) || (id[1] == 0x11));
}

static bool lcd_read_register(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t err;

    if ((data == NULL) || (len == 0)) {
        return false;
    }

    err = lcd_panel_read(reg, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read reg 0x%02X failed over SPI transport: %s", reg, esp_err_to_name(err));
        return false;
    }

    return true;
}

static void lcd_read_id_rdidx(uint8_t id[3])
{
    if (id == NULL) {
        return;
    }

    if (!lcd_read_register(ICNA3312_RDID1, &id[0], 1) ||
        !lcd_read_register(ICNA3312_RDID2, &id[1], 1) ||
        !lcd_read_register(ICNA3312_RDID3, &id[2], 1)) {
        id[0] = 0;
        id[1] = 0;
        id[2] = 0;
    }
}

static void lcd_read_id_rddid(uint8_t id[3])
{
    if ((id == NULL) || !lcd_read_register(ICNA3312_RDDID, id, 3)) {
        if (id != NULL) {
            id[0] = 0;
            id[1] = 0;
            id[2] = 0;
        }
    }
}

static bool lcd_read_id_rdddbs(uint8_t id[5])
{
    if (id == NULL) {
        return false;
    }

    if (!lcd_read_register(ICNA3312_RDDDBS, id, 5)) {
        for (size_t index = 0; index < 5; index++) {
            id[index] = 0;
        }
        return false;
    }

    return true;
}

static void lcd_try_read_id(void)
{
    uint8_t local_id[3] = {0};
    uint8_t display_db[5] = {0};

    if (SPI_MISO_GPIO_PIN < 0) {
        ESP_LOGW(TAG, "panel readback disabled because SPI MISO is not configured");
        return;
    }

    if (lcd_read_id_rdddbs(display_db)) {
        ESP_LOGI(TAG, "probe RDDDBS via SPI MISO gpio%d -> %02X %02X %02X %02X %02X",
                 SPI_MISO_GPIO_PIN,
                 display_db[0], display_db[1], display_db[2], display_db[3], display_db[4]);

        local_id[0] = display_db[0];
        local_id[1] = display_db[1];
        local_id[2] = display_db[2];
        if (lcd_id_looks_valid(local_id)) {
            lcd_id_cache[0] = local_id[0];
            lcd_id_cache[1] = local_id[1];
            lcd_id_cache[2] = local_id[2];
            lcd_id_valid = true;
            return;
        }
    }

    lcd_read_id_rddid(local_id);
    ESP_LOGI(TAG, "probe RDDID via SPI MISO gpio%d -> ID %02X %02X %02X",
             SPI_MISO_GPIO_PIN, local_id[0], local_id[1], local_id[2]);
    if (lcd_id_looks_valid(local_id)) {
        lcd_id_cache[0] = local_id[0];
        lcd_id_cache[1] = local_id[1];
        lcd_id_cache[2] = local_id[2];
        lcd_id_valid = true;
        return;
    }

    lcd_read_id_rdidx(local_id);
    ESP_LOGI(TAG, "probe RDIDx via SPI MISO gpio%d -> ID %02X %02X %02X",
             SPI_MISO_GPIO_PIN, local_id[0], local_id[1], local_id[2]);
    if (lcd_id_looks_valid(local_id)) {
        lcd_id_cache[0] = local_id[0];
        lcd_id_cache[1] = local_id[1];
        lcd_id_cache[2] = local_id[2];
        lcd_id_valid = true;
        return;
    }

    ESP_LOGW(TAG, "panel ID readback did not match expected 33 11/10 00; continuing anyway");
}

static void lcd_log_status_registers(const char *stage)
{
    uint8_t rddst = 0;
    uint8_t rddim = 0;
    uint8_t brightness = 0;

    if (!lcd_read_register(ICNA3312_RDDST, &rddst, 1) ||
        !lcd_read_register(ICNA3312_RDDIM, &rddim, 1) ||
        !lcd_read_register(ICNA3312_RDDISBV, &brightness, 1)) {
        ESP_LOGW(TAG, "%s status readback unavailable over SPI transport", stage);
        return;
    }

    ESP_LOGI(TAG,
             "%s RDDST=0x%02X (sleep_out=%u normal=%u display_on=%u booster=%u)",
             stage, rddst,
             (rddst >> 4) & 0x01,
             (rddst >> 3) & 0x01,
             (rddst >> 2) & 0x01,
             (rddst >> 7) & 0x01);
    ESP_LOGI(TAG, "%s RDDIM=0x%02X (allpon=%u allpoff=%u inversion=%u)",
             stage, rddim,
             (rddim >> 4) & 0x01,
             (rddim >> 3) & 0x01,
             (rddim >> 5) & 0x01);
    ESP_LOGI(TAG, "%s RDDISBV=0x%02X", stage, brightness);
}

static void lcd_bus_init(void)
{
    esp_err_t err;
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .spics_io_num = LCD_NUM_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    if (lcd_bus_ready) {
        return;
    }

    spi2_init();
    err = spi2_add_device(&devcfg, &MY_LCD_Handle);
    ESP_ERROR_CHECK(err);

    lcd_bus_ready = true;
    ESP_LOGI(TAG, "SPI transport ready: host=SPI2 clk=%dHz mosi=%d miso=%d sclk=%d cs=%d",
             LCD_SPI_CLOCK_HZ, SPI_MOSI_GPIO_PIN, SPI_MISO_GPIO_PIN, SPI_CLK_GPIO_PIN, LCD_NUM_CS);
}

void lcd_write_cmd(uint8_t cmd)
{
    if (!lcd_bus_ready) {
        return;
    }

    lcd_flush_pending_cmd();
    lcd_memory_write_active = false;

    if (!lcd_cmd_accepts_payload(cmd)) {
        esp_err_t err = lcd_panel_write(cmd, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
        }
        return;
    }

    lcd_pending_cmd = cmd;
    lcd_pending_cmd_valid = true;
}

void lcd_write_data(const uint8_t *data, int len)
{
    esp_err_t err;

    if (!lcd_bus_ready || (data == NULL) || (len <= 0)) {
        return;
    }

    if (lcd_pending_cmd_valid) {
        const uint8_t cmd = lcd_pending_cmd;

        lcd_pending_cmd_valid = false;
        err = lcd_panel_write(cmd, data, (size_t)len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write cmd 0x%02X payload failed: %s", cmd, esp_err_to_name(err));
        }
        lcd_memory_write_active = lcd_is_memory_write_cmd(cmd);
        return;
    }

    if (lcd_memory_write_active) {
        err = lcd_panel_write(ICNA3312_RAMWRC, data, (size_t)len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write RAMWRC payload failed: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGW(TAG, "ignore lcd_write_data without active command");
}

void lcd_write_data16(uint16_t data)
{
    uint8_t pixel[2];

    pixel[0] = (uint8_t)(data >> 8);
    pixel[1] = (uint8_t)data;
    lcd_write_data(pixel, sizeof(pixel));
}

void lcd_read_id(uint8_t id[3])
{
    if (id == NULL) {
        return;
    }

    if (!lcd_id_valid) {
        lcd_try_read_id();
    }

    id[0] = lcd_id_cache[0];
    id[1] = lcd_id_cache[1];
    id[2] = lcd_id_cache[2];
}

void lcd_set_window(uint16_t xstar, uint16_t ystar, uint16_t xend, uint16_t yend)
{
    uint8_t databuf[4];

    if (!lcd_bus_ready) {
        return;
    }

    if ((xstar > xend) || (ystar > yend)) {
        return;
    }

    databuf[0] = (uint8_t)(xstar >> 8);
    databuf[1] = (uint8_t)xstar;
    databuf[2] = (uint8_t)(xend >> 8);
    databuf[3] = (uint8_t)xend;
    lcd_send_cmd_params(lcd_self.setxcmd, databuf, sizeof(databuf));

    databuf[0] = (uint8_t)(ystar >> 8);
    databuf[1] = (uint8_t)ystar;
    databuf[2] = (uint8_t)(yend >> 8);
    databuf[3] = (uint8_t)yend;
    lcd_send_cmd_params(lcd_self.setycmd, databuf, sizeof(databuf));

    lcd_write_cmd(lcd_self.wramcmd);
}

void lcd_set_cursor(uint16_t xpos, uint16_t ypos)
{
    lcd_set_window(xpos, ypos, xpos, ypos);
}

void lcd_clear(uint16_t color)
{
    lcd_fill(0, 0, lcd_self.width - 1, lcd_self.height - 1, color);
}

void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    uint32_t total_pixels;
    uint32_t chunk_pixels;

    if (!lcd_bus_ready) {
        return;
    }

    if ((sx > ex) || (sy > ey) || (ex >= lcd_self.width) || (ey >= lcd_self.height)) {
        return;
    }

    for (int index = 0; index < LCD_BUF_SIZE; index += 2) {
        lcd_buf[index] = (uint8_t)(color >> 8);
        lcd_buf[index + 1] = (uint8_t)color;
    }

    total_pixels = (uint32_t)(ex - sx + 1) * (uint32_t)(ey - sy + 1);
    lcd_set_window(sx, sy, ex, ey);

    while (total_pixels > 0) {
        chunk_pixels = total_pixels;
        if (chunk_pixels > (LCD_BUF_SIZE / 2U)) {
            chunk_pixels = LCD_BUF_SIZE / 2U;
        }

        lcd_write_data(lcd_buf, (int)(chunk_pixels * 2U));
        total_pixels -= chunk_pixels;
    }

    lcd_finish_memory_write();
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if ((x >= lcd_self.width) || (y >= lcd_self.height)) {
        return;
    }

    lcd_set_cursor(x, y);
    lcd_write_data16(color);
    lcd_finish_memory_write();
}

void lcd_scan_dir(uint8_t dir)
{
    uint8_t regval;
    uint16_t temp;

    switch (dir) {
        case L2R_U2D: regval = 0x00; break;
        case L2R_D2U: regval = 0x80; break;
        case R2L_U2D: regval = 0x40; break;
        case R2L_D2U: regval = 0xC0; break;
        case U2D_L2R: regval = 0x20; break;
        case U2D_R2L: regval = 0x60; break;
        case D2U_L2R: regval = 0xA0; break;
        case D2U_R2L: regval = 0xE0; break;
        default:
            ESP_LOGW(TAG, "invalid scan direction %u, fallback to default", dir);
            regval = 0x00;
            dir = DFT_SCAN_DIR;
            break;
    }

    if (lcd_bus_ready) {
        lcd_send_cmd_params(ICNA3312_MADCTL, &regval, 1);
    }

    if (lcd_scan_dir_swaps_axes(dir)) {
        if (lcd_self.width < lcd_self.height) {
            temp = lcd_self.width;
            lcd_self.width = lcd_self.height;
            lcd_self.height = temp;
        }
    } else if (lcd_self.width > lcd_self.height) {
        temp = lcd_self.width;
        lcd_self.width = lcd_self.height;
        lcd_self.height = temp;
    }
}

void lcd_display_dir(uint8_t dir)
{
    if (dir > D2U_R2L) {
        ESP_LOGW(TAG, "invalid display direction %u, fallback to default", dir);
        dir = DFT_SCAN_DIR;
    }

    lcd_update_display_state(lcd_scan_dir_swaps_axes(dir));

    if (lcd_bus_ready) {
        lcd_scan_dir(dir);
    }
}

void lcd_minimal_init_sequence(void)
{
    ESP_LOGI(TAG, "send SLPOUT and wait %d ms", LCD_SLPOUT_DELAY_MS);
    lcd_write_cmd(ICNA3312_SLPOUT);
    lcd_delay_ms(LCD_SLPOUT_DELAY_MS);
}

void lcd_run_white_smoke_test(void)
{
    uint8_t data;

    if (!lcd_bus_ready) {
        return;
    }

    data = 0x28;
    lcd_send_cmd_params(ICNA3312_WRCTRLD, &data, 1);

    data = 0xFF;
    lcd_send_cmd_params(ICNA3312_WRDISBV, &data, 1);

    data = 0x00;
    lcd_send_cmd_params(ICNA3312_WRACL, &data, 1);

    ESP_LOGI(TAG, "white smoke test: WRCTRLD=0x28 WRDISBV=0xFF WRACL=0x00 DISPON ALLPON");
    lcd_write_cmd(ICNA3312_DISPON);
    lcd_delay_ms(LCD_DISPON_DELAY_MS);
    lcd_write_cmd(ICNA3312_ALLPON);
    lcd_delay_ms(5);

    lcd_log_status_registers("after white");
}

void lcd_force_white(void)
{
    lcd_run_white_smoke_test();
}

void lcd_init(void)
{
    uint8_t panel_id[3] = {0};

    if (lcd_initialized) {
        return;
    }

    lcd_display_dir(DFT_SCAN_DIR);
    lcd_panel_sideband_gpio_init();
    ds4730_panel_power_on_sequence();
    lcd_bus_init();

    ESP_LOGI(TAG, "ICNA3312 bring-up pins: MOSI=%d MISO=%d SCLK=%d CS=%d SIO1=%d SIO2=%d SIO3=%d",
             SPI_MOSI_GPIO_PIN, SPI_MISO_GPIO_PIN, SPI_CLK_GPIO_PIN, LCD_NUM_CS,
             LCD_NUM_SIO1, LCD_NUM_SIO2, LCD_NUM_SIO3);

    /* Give the panel time after the final hard reset before sending command traffic. */
    lcd_delay_ms(LCD_RESET_TO_FIRST_CMD_DELAY_MS);

    ESP_LOGI(TAG, "starting ICNA3312 SPI command-link probe");
    lcd_try_read_id();
    lcd_log_status_registers("after power");
    lcd_minimal_init_sequence();
    lcd_display_dir(DFT_SCAN_DIR);
    lcd_log_status_registers("after SLPOUT");
    lcd_read_id(panel_id);

    lcd_initialized = true;
    ESP_LOGI(TAG, "panel ready: %ux%u, ID=%02X %02X %02X",
             lcd_self.width, lcd_self.height, panel_id[0], panel_id[1], panel_id[2]);
}
