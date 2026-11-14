#include "delay_LL.h"


#ifdef USE_RTOS2
#include "cmsis_os2.h"
#endif

__IO uint32_t msTick = 0; //msec tick


void Systick_LL_Init(void){

   LL_SYSTICK_SetClkSource( LL_SYSTICK_CLKSOURCE_HCLK);
   LL_Init1msTick(SystemCoreClock);
   LL_SYSTICK_EnableIT();
   NVIC_SetPriority(SysTick_IRQn, 15); //
  // LL_mDelay(10);


}

#ifndef USE_RTOS2 
void SysTick_Handler(void){

  msTick++;

}
#endif


uint32_t GetSysTick(void) {

  #ifdef USE_RTOS2
    return osKernelGetTickCount();
  #else
    return msTick;
  #endif

}

//delay ms
void DelayMs(uint32_t delay_ms) {

  uint32_t start = GetSysTick();
  while ((GetSysTick() - start) < delay_ms);

}