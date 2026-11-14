#ifndef I2S_LL_H
#define I2S_LL_H

#include "app_main.h"

// Увеличили блок DMA с 64 до 256 моно-сэмплов (16 мс), так как USB больше не нужен
#define MONO_PER_BUF      256U
#define DMA_BUF_LEN       (MONO_PER_BUF * 2U)     // 512 int16_t

#define FLAG_BUF_A        (1U << 0)               // 0x01U
#define FLAG_BUF_B        (1U << 1)               // 0x02U

extern __IO int16_t buffer_dma_A[DMA_BUF_LEN];
extern __IO int16_t buffer_dma_B[DMA_BUF_LEN];
extern osThreadId_t g_mic_tid;

void I2S_Init(void);
void HSE_SetPLLI2S_16kHz(void);

#endif