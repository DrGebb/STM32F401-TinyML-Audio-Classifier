#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "RTE_Components.h"
#include CMSIS_device_header
#include "cmsis_os2.h"

#include <stdio.h>
#include <string.h>
#include "stm32f4xx_ll_gpio.h"

#include "stm32f4xx_ll_spi.h"
#include <stdint.h>
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_rcc.h"
#include "NanoEdgeAI.h"
#include <stdbool.h>




void app_main(void);

#endif