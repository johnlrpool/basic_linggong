#ifndef __DS4730_H__
#define __DS4730_H__

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_NUM_RST                GPIO_NUM_5
#define PANEL_NUM_IM1                GPIO_NUM_38
#define PANEL_NUM_IM0                GPIO_NUM_39
#define PANEL_NUM_SWIRE              GPIO_NUM_1
#define PANEL_NUM_VDDIO_1V8          GPIO_NUM_21
#define PANEL_NUM_AVDD_2V8           GPIO_NUM_47
#define PANEL_NUM_RT4730_EN          GPIO_NUM_14
#define PANEL_NUM_VCI_EN             PANEL_NUM_RT4730_EN

void ds4730_panel_hard_reset(void);
void ds4730_panel_power_on_sequence(void);

#ifdef __cplusplus
}
#endif

#endif
