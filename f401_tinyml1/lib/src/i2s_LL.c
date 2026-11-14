#include "i2s_LL.h"


//for dma
__IO int16_t buffer_dma_A[DMA_BUF_LEN];
__IO int16_t buffer_dma_B[DMA_BUF_LEN];

__IO uint32_t isr_countA, isr_countB;//for test-debug


//for I2S
void HSE_SetPLLI2S_16kHz(void){

    LL_RCC_PLLI2S_Disable();
    LL_RCC_PLLI2S_ConfigDomain_I2S(
        LL_RCC_PLLSOURCE_HSE,
        LL_RCC_PLLI2SM_DIV_25,  
        256,                     
        LL_RCC_PLLI2SR_DIV_5    
    );
    LL_RCC_PLLI2S_Enable();
    while (LL_RCC_PLLI2S_IsReady() != 1) __NOP();

    // Src internal I2S clock = PLLI2S
    LL_RCC_SetI2SClockSource(LL_RCC_I2S1_CLKSOURCE_PLLI2S);

}



void I2S_Init(void){

    //en clk for SPI2 and GPIOB
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB);

    // PB15 - SD, PB12 - WS, PB13 - CK
    LL_GPIO_InitTypeDef GPIO_init = {0};
    GPIO_init.Pin = LL_GPIO_PIN_15 | LL_GPIO_PIN_12 | LL_GPIO_PIN_13;
    GPIO_init.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_init.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_init.Pull = LL_GPIO_PULL_NO;
    GPIO_init.Alternate = LL_GPIO_AF_5;
    GPIO_init.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    LL_GPIO_Init(GPIOB, &GPIO_init);

    //Config I2S 16kHz, 16bit
    // LL_I2S_InitTypeDef I2S_init = {0};
    // I2S_init.Mode = LL_I2S_MODE_MASTER_RX;
    // I2S_init.Standard = LL_I2S_STANDARD_PHILIPS;
    // I2S_init.DataFormat = LL_I2S_DATAFORMAT_16B_EXTENDED;
    // I2S_init.MCLKOutput = LL_I2S_MCLK_OUTPUT_DISABLE;
    // I2S_init.AudioFreq = LL_I2S_AUDIOFREQ_16K;
    // I2S_init.ClockPolarity = LL_I2S_POLARITY_LOW;
    // LL_I2S_Init(SPI2, &I2S_init);

    LL_I2S_SetTransferMode(SPI2, LL_I2S_MODE_MASTER_RX);
    LL_I2S_SetStandard(SPI2, LL_I2S_STANDARD_PHILIPS);
    LL_I2S_SetDataFormat(SPI2, LL_I2S_DATAFORMAT_16B_EXTENDED);
    LL_I2S_DisableMasterClock(SPI2);
    LL_I2S_SetClockPolarity(SPI2, LL_I2S_POLARITY_LOW);
    LL_I2S_SetPrescalerLinear(SPI2, 25);                             // DIV=25
    LL_I2S_SetPrescalerParity(SPI2, LL_I2S_PRESCALER_PARITY_EVEN);  // ODD=0
    

    //en dma RX
    LL_I2S_EnableDMAReq_RX(SPI2);
    

    //config DMA1, SPI2_RX - stream3 ch0
     LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

        //DMA conf
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_3);
        //wait if stream en
    while (LL_DMA_IsEnabledStream(DMA1,LL_DMA_STREAM_3 )) __NOP();
        //reset flags
    LL_DMA_ClearFlag_HT3(DMA1);
    LL_DMA_ClearFlag_TC3(DMA1);
    LL_DMA_ClearFlag_FE3(DMA1);
    LL_DMA_ClearFlag_TE3(DMA1);
    LL_DMA_ClearFlag_DME3(DMA1);
        //fill sruct
    LL_DMA_InitTypeDef dma_init = {0};
    dma_init.Channel = LL_DMA_CHANNEL_0;
    dma_init.Mode = LL_DMA_MODE_CIRCULAR;
    dma_init.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    dma_init.PeriphOrM2MSrcAddress = (uint32_t)&(SPI2->DR);//i2s1 src need
    dma_init.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
    dma_init.MemoryOrM2MDstAddress = (uint32_t)buffer_dma_A; //1st bufferA
    dma_init.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
    dma_init.NbData = DMA_BUF_LEN;
    dma_init.PeriphBurst = LL_DMA_PBURST_SINGLE;
    dma_init.MemBurst = LL_DMA_MBURST_SINGLE;
    dma_init.FIFOMode = LL_DMA_FIFOMODE_ENABLE;
    dma_init.FIFOThreshold = LL_DMA_FIFOTHRESHOLD_1_4;
    dma_init.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    dma_init.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    dma_init.Priority = LL_DMA_PRIORITY_HIGH;
    LL_DMA_Init(DMA1, LL_DMA_STREAM_3, &dma_init);


    //double buffer config
    LL_DMA_EnableDoubleBufferMode(DMA1, LL_DMA_STREAM_3);
    LL_DMA_SetMemory1Address(DMA1, LL_DMA_STREAM_3, (uint32_t)buffer_dma_B);

    //IT
    LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_3); //Transfer compl IT
    NVIC_EnableIRQ(DMA1_Stream3_IRQn);        

    //en DMA
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_3);


    //en I2S
    LL_I2S_Enable(SPI2);




}

void reStartDMA_I2S_transmit(){

    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_3);
        //wait if stream en
    while (LL_DMA_IsEnabledStream(DMA1,LL_DMA_STREAM_3 )) __NOP();
         //reset flags
    LL_DMA_ClearFlag_HT3(DMA1);
    LL_DMA_ClearFlag_TC3(DMA1);
    LL_DMA_ClearFlag_FE3(DMA1);
    LL_DMA_ClearFlag_TE3(DMA1);
    LL_DMA_ClearFlag_DME3(DMA1);

    // reset to CT=0 → DMA start with  M0AR (buffer_A)
    LL_DMA_SetCurrentTargetMem(DMA1, LL_DMA_STREAM_3, LL_DMA_CURRENTTARGETMEM0);

    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_3, DMA_BUF_LEN);
    LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_3, (uint32_t)buffer_dma_A);
    LL_DMA_SetMemory1Address(DMA1, LL_DMA_STREAM_3, (uint32_t)buffer_dma_B);

    //en DMA
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_3);

}

void DMA1_Stream3_IRQHandler(void) {

    if (LL_DMA_IsActiveFlag_TC3(DMA1)) {
        LL_DMA_ClearFlag_TC3(DMA1);
        if (LL_DMA_GetCurrentTargetMem(DMA1, LL_DMA_STREAM_3) == LL_DMA_CURRENTTARGETMEM0) {
            isr_countB++;
            osThreadFlagsSet(g_mic_tid, FLAG_BUF_B);   // B ready
        } else {
            isr_countA++;
            osThreadFlagsSet(g_mic_tid, FLAG_BUF_A);   // A ready
        }
    }
}

void testGetDataFromBufferAB(int16_t * arr){

    for(uint8_t i = 0; i < 40; i++){

        if (i < 20) {
            arr[i] = buffer_dma_A[i];
        }
        else {
            arr[i] = buffer_dma_B[i];
        }
    }
}