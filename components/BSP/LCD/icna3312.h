#ifndef __ICNA3312_H__
#define __ICNA3312_H__

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICNA3312_LCD_W               450
#define ICNA3312_LCD_H               600

#define LCD_NUM_TE                   GPIO_NUM_4
#define LCD_NUM_CS                   GPIO_NUM_17
#define LCD_NUM_SCL                  GPIO_NUM_16
#define LCD_NUM_SIO0                 GPIO_NUM_7
#define LCD_NUM_SIO1                 GPIO_NUM_15
#define LCD_NUM_SIO2                 GPIO_NUM_18
#define LCD_NUM_SIO3                 GPIO_NUM_8
#define LCD_NUM_SDO                  GPIO_NUM_6
#define LCD_NUM_UNUSED_IO2           GPIO_NUM_3
#define LCD_NUM_UNUSED_IO3           GPIO_NUM_46

#define WHITE                        0xFFFF
#define BLACK                        0x0000
#define RED                          0xF800
#define GREEN                        0x07E0
#define BLUE                         0x001F
#define MAGENTA                      0xF81F
#define YELLOW                       0xFFE0
#define CYAN                         0x07FF

#define ICNA3312_NOP                 0x00
#define ICNA3312_SWRESET             0x01
#define ICNA3312_RDDID               0x04
#define ICNA3312_RDDPM               0x0A
#define ICNA3312_RDDST               0x09
#define ICNA3312_SLPIN               0x10
#define ICNA3312_SLPOUT              0x11
#define ICNA3312_NORON               0x13
#define ICNA3312_DISPOFF             0x28
#define ICNA3312_DISPON              0x29
#define ICNA3312_CASET               0x2A
#define ICNA3312_RASET               0x2B
#define ICNA3312_RAMWR               0x2C
#define ICNA3312_RAMWRC              0x3C
#define ICNA3312_ALLPOFF             0x22
#define ICNA3312_ALLPON              0x23
#define ICNA3312_TEOFF               0x34
#define ICNA3312_TEON                0x35
#define ICNA3312_MADCTL              0x36
#define ICNA3312_COLMOD              0x3A
#define ICNA3312_WRDISBV             0x51
#define ICNA3312_RDDISBV             0x52
#define ICNA3312_WRCTRLD             0x53
#define ICNA3312_RDCTRLD             0x54
#define ICNA3312_WRACL               0x55
#define ICNA3312_RDDIM               0x0D
#define ICNA3312_SETDISPMODE         0xC2
#define ICNA3312_SETDSPIMODE         0xC4
#define ICNA3312_RDDDBS              0xA1
#define ICNA3312_RDID1               0xDA
#define ICNA3312_RDID2               0xDB
#define ICNA3312_RDID3               0xDC
#define ICNA3312_PAGESEL             0xFE
#define ICNA3312_PAGESTAT            0xFF

#define L2R_U2D                      0
#define L2R_D2U                      1
#define R2L_U2D                      2
#define R2L_D2U                      3
#define U2D_L2R                      4
#define U2D_R2L                      5
#define D2U_L2R                      6
#define D2U_R2L                      7
#define DFT_SCAN_DIR                 L2R_U2D

#define LCD_BUF_SIZE                 4092

typedef struct _lcd_obj_t
{
    uint16_t width;
    uint16_t height;
    uint8_t dir;
    uint16_t wramcmd;
    uint16_t setxcmd;
    uint16_t setycmd;
    uint16_t cs;
} lcd_obj_t;

extern lcd_obj_t lcd_self;
extern uint8_t lcd_buf[LCD_BUF_SIZE];
extern spi_device_handle_t MY_LCD_Handle;

void lcd_init(void);
void lcd_clear(uint16_t color);
void lcd_display_dir(uint8_t dir);
void lcd_scan_dir(uint8_t dir);
void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(const uint8_t *data, int len);
void lcd_write_data16(uint16_t data);
void lcd_set_cursor(uint16_t xpos, uint16_t ypos);
void lcd_set_window(uint16_t xstar, uint16_t ystar, uint16_t xend, uint16_t yend);
void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void lcd_minimal_init_sequence(void);
void lcd_read_id(uint8_t id[3]);
void lcd_run_white_smoke_test(void);
void lcd_force_white(void);

#ifdef __cplusplus
}
#endif

#endif
