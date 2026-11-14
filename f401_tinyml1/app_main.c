#include "app_main.h"
#include "i2s_LL.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"





//LED PC13 (configed in MX_GPIO_Init)
__STATIC_INLINE void LED_Set(bool state)
{
    if (state) {
        GPIOC->BSRR = (1U << (13U + 16U)); // ON
    } else {
        GPIOC->BSRR = (1U << 13U);         // OFF
    }
}

//Microphone L/R contact PB3
//(there is no available GND contact on the development board)

#define USE_PIN_FOR_MIC_LR
#ifdef USE_PIN_FOR_MIC_LR
void mic_pin_lr()
{

  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB);

  LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_3);
  
  GPIO_InitStruct.Pin = LL_GPIO_PIN_3;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_3);

}
#endif




/* === Gain and DC filter settings (strictly as during dataset collection!) === */
#define MIC_GAIN            14.0f
#define MIC_GAIN_Q8         ((int32_t)(MIC_GAIN * 256.0f))
#define MIC_DC_SHIFT        7

_Static_assert(MIC_GAIN_Q8 > 0 && MIC_GAIN_Q8 <= 32768, "MIC_GAIN must be in 0..128");

/* === Window and 50% overlap parameters === */
#define WINDOW_LEN          NEAI_INPUT_SIGNAL_LENGTH          // 1024 (or 4096) from NanoEdgeAI.h
#define STEP_LEN            (WINDOW_LEN / 2U)                 // 50% overlap: 512 (or 2048)

#define FLAG_NEAI_READY     (1U << 0)                         // Flag to start inference

/* Class indices from your metadata.json */
#define CLASS_SPICE         0
#define CLASS_SILENT        1
#define CLASS_PACKET        2
#define CLASS_NOISE         3

/* === Global variables and buffers === */
osThreadId_t g_mic_tid;
osThreadId_t g_neai_tid;

static int32_t  g_dc_q8   = 0;
static bool     g_dc_init = false;

__IO uint32_t mic_overrun = 0;
__IO uint32_t mic_clip    = 0;
__IO uint32_t neai_busy_drop = 0; // If the classifier did not finish before the next step arrived

// Half-window buffer (accumulated in mic_thread)
static float step_buffer[STEP_LEN];
// Working window to pass to neai_classification()
static float neai_input_buf[WINDOW_LEN];
// Busy flag for neai_input_buf buffer
static __IO bool neai_processing = false;

/* Classification results */
__IO int   g_raw_class_id = CLASS_SILENT;
__IO float g_probabilities[NEAI_NUMBER_OF_CLASSES];
__IO int   g_final_detected_class = CLASS_SILENT; // Final result after voting
__IO uint32_t g_inference_count = 0;


/* ============================================================================
 * TASK 1: Audio capture from DMA, DC filtering, gain, and window assembly (High Priority)
 * ============================================================================ */
static void mic_thread(void *arg)
{
    (void)arg;

    #ifdef USE_PIN_FOR_MIC_LR
    mic_pin_lr();
    #endif

    uint32_t step_idx = 0;
    bool first_half_filled = false;

    g_mic_tid = osThreadGetId();

    // Start I2S + DMA only after getting the thread ID
    I2S_Init();

    for (;;) {
        uint32_t f = osThreadFlagsWait(FLAG_BUF_A | FLAG_BUF_B, osFlagsWaitAny, osWaitForever);
        if (f & osFlagsError) { continue; }

        if ((f & FLAG_BUF_A) && (f & FLAG_BUF_B)) {
            mic_overrun++;
        }

        for (int b = 0; b < 2; b++) {
            if (!(f & (b ? FLAG_BUF_B : FLAG_BUF_A))) { continue; }

            const __IO int16_t *src = b ? buffer_dma_B : buffer_dma_A;

            // Exact same DSP pipeline used when recording the dataset!
            for (uint32_t i = 0; i < MONO_PER_BUF; i++) {
                int32_t s = src[2U * i]; // Even indices = Left channel

                if (!g_dc_init) {
                    g_dc_q8 = s << 8;
                    g_dc_init = true;
                }
                g_dc_q8 += ((s << 8) - g_dc_q8) >> MIC_DC_SHIFT;
                s -= (g_dc_q8 >> 8);

                int32_t v = (s * MIC_GAIN_Q8) >> 8;
                int32_t o = __SSAT(v, 16);
                if (o != v) { mic_clip++; }

                // Convert int16_t -> float (hardware instruction VCVT.F32.S32 on FPU)
                step_buffer[step_idx++] = (float)((int16_t)o);

                // As soon as exactly half a window (STEP_LEN samples) is accumulated
                if (step_idx >= STEP_LEN) {
                    step_idx = 0;

                    if (!first_half_filled) {
                        // Very first time after startup, fill the first half of the window
                        memcpy(&neai_input_buf[0], step_buffer, STEP_LEN * sizeof(float));
                        first_half_filled = true;
                    } else {
                        // Check if the classifier is free
                        if (!neai_processing) {
                            neai_processing = true;

                            // 1. Shift the second half of the window to the first half (50% Overlap)
                            memmove(&neai_input_buf[0], &neai_input_buf[STEP_LEN], STEP_LEN * sizeof(float));
                            // 2. Copy the fresh half into the second part of the window
                            memcpy(&neai_input_buf[STEP_LEN], step_buffer, STEP_LEN * sizeof(float));

                            // 3. Wake up the classification task!
                            osThreadFlagsSet(g_neai_tid, FLAG_NEAI_READY);
                        } else {
                            neai_busy_drop++;
                        }
                    }
                }
            }
        }
    }
}


/* ============================================================================
 * TASK 2: NanoEdge AI classification and voting
 * ============================================================================ */
// 1. History length: 32 steps of 32 ms = 1.024 seconds
#define VOTE_HISTORY_LEN            32U

// 2. Single-window confidence thresholds (0.0 .. 1.0)
#define CONF_THRESH_SPICE           0.80f
#define CONF_THRESH_PACKET          0.75f

// 3. SEPARATE frame count thresholds (out of 32):
// To trigger SPICE, at least 16 purely SPICE frames (~0.5 sec) are needed
#define MIN_SPICE_FRAMES            14U
#define MIN_PACKET_FRAMES           10U

// 4. Minimum sum of probabilities over 1 second
#define MIN_SCORE_SPICE             10.0f
#define MIN_SCORE_PACKET            8.0f

// 5. VETO: If there were more than 2 PACKET frames in the last second — it is NOT spices!
#define MAX_PACKET_FRAMES_FOR_SPICE 2U

// 6. TAIL LOCKOUT (Cooldown): how many 32 ms steps after PACKET
// it is forbidden to trigger SPICE (25 steps ≈ 0.8 seconds for the bag to settle)
#define PACKET_COOLDOWN_STEPS       25U

// 7. Protection against background noise
#define NOISE_PENALTY_FACTOR        1.5f

/* Variables for debugging in Keil Watch */
__IO uint32_t dbg_spice_cnt  = 0;
__IO uint32_t dbg_packet_cnt = 0;
__IO uint32_t dbg_noise_cnt  = 0;
__IO uint32_t dbg_cooldown   = 0;
__IO float    dbg_sum_spice  = 0.0f;
__IO float    dbg_sum_packet = 0.0f;

//profiler
__IO uint32_t time_execut_class_us = 0;


static void neai_thread(void *arg)
{
    (void)arg;
    
    //profiler
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL       |= DWT_CTRL_CYCCNTENA_Msk;

    float local_probs[NEAI_NUMBER_OF_CLASSES];
    int   local_class_id = 0;

    // History ring buffers with SEPARATE counters for spice and packet
    float   hist_spice[VOTE_HISTORY_LEN]     = {0};
    float   hist_packet[VOTE_HISTORY_LEN]    = {0};
    float   hist_noise[VOTE_HISTORY_LEN]     = {0};
    uint8_t hist_is_spice[VOTE_HISTORY_LEN]  = {0};
    uint8_t hist_is_packet[VOTE_HISTORY_LEN] = {0};
    uint8_t hist_is_noise[VOTE_HISTORY_LEN]  = {0};
    uint32_t hist_idx = 0;

    // Lockout timer for SPICE after rustling a large bag stops
    uint32_t packet_cooldown = 0;

    g_neai_tid = osThreadGetId();

    enum neai_state state = neai_classification_init();
    if (state != NEAI_OK) {
        for (;;) { osDelay(1000); }
    }

    for (;;) {
        uint32_t f = osThreadFlagsWait(FLAG_NEAI_READY, osFlagsWaitAny, osWaitForever);
        if (f & osFlagsError) { continue; }

        //start prof
        uint32_t start_cycles = DWT->CYCCNT;

        state = neai_classification(neai_input_buf, local_probs, &local_class_id);

        //stop prof
        uint32_t elapsed_cycles = DWT->CYCCNT - start_cycles;
        time_execut_class_us = elapsed_cycles / (SystemCoreClock / 1000000U);


        neai_processing = false;

        if (state == NEAI_OK) {
            g_inference_count++;
            g_raw_class_id = local_class_id;
            for (int c = 0; c < NEAI_NUMBER_OF_CLASSES; c++) {
                g_probabilities[c] = local_probs[c];
            }

            float p_spice  = local_probs[CLASS_SPICE];
            float p_packet = local_probs[CLASS_PACKET];
            float p_noise  = local_probs[CLASS_NOISE];

            // Clear the current cell of the ring buffer
            hist_spice[hist_idx]     = 0.0f;
            hist_packet[hist_idx]    = 0.0f;
            hist_noise[hist_idx]     = 0.0f;
            hist_is_spice[hist_idx]  = 0U;
            hist_is_packet[hist_idx] = 0U;
            hist_is_noise[hist_idx]  = 0U;

            // Fill separate statistics per class
            if (p_spice >= CONF_THRESH_SPICE) {
                hist_spice[hist_idx]    = p_spice;
                hist_is_spice[hist_idx] = 1U;
            } 
            else if (p_packet >= CONF_THRESH_PACKET) {
                hist_packet[hist_idx]    = p_packet;
                hist_is_packet[hist_idx] = 1U;
            } 
            else if (p_noise > 0.50f || (p_spice > 0.35f && p_spice < CONF_THRESH_SPICE)) {
                hist_noise[hist_idx]    = p_noise + p_spice;
                hist_is_noise[hist_idx] = 1U;
            }

            hist_idx = (hist_idx + 1U) % VOTE_HISTORY_LEN;

            // Calculate separate sums over the last 1 second (32 steps)
            uint32_t spice_cnt  = 0;
            uint32_t packet_cnt = 0;
            uint32_t noise_cnt  = 0;
            float sum_spice  = 0.0f;
            float sum_packet = 0.0f;
            float sum_noise  = 0.0f;

            for (uint32_t k = 0; k < VOTE_HISTORY_LEN; k++) {
                spice_cnt  += hist_is_spice[k];
                packet_cnt += hist_is_packet[k];
                noise_cnt  += hist_is_noise[k];
                sum_spice  += hist_spice[k];
                sum_packet += hist_packet[k];
                sum_noise  += hist_noise[k];
            }

            // Cooldown logic: if there is confident PACKET in the window (at least 4 frames),
            // arm the SPICE lockout timer so the "tail" of the bag when releasing the hand does not trigger SPICE
            if (packet_cnt >= 4U) {
                packet_cooldown = PACKET_COOLDOWN_STEPS;
            } else if (packet_cooldown > 0U) {
                packet_cooldown--;
            }

            // Debug for Keil Watch
            dbg_spice_cnt  = spice_cnt;
            dbg_packet_cnt = packet_cnt;
            dbg_noise_cnt  = noise_cnt;
            dbg_cooldown   = packet_cooldown;
            dbg_sum_spice  = sum_spice;
            dbg_sum_packet = sum_packet;

            /* --- Final decision making --- */
            // 1. Check PACKET first
            if (packet_cnt >= MIN_PACKET_FRAMES &&
                sum_packet >= MIN_SCORE_PACKET &&
                sum_packet > (sum_noise * NOISE_PENALTY_FACTOR))
            {
                g_final_detected_class = CLASS_PACKET;
            }
            // 2. Check SPICE only if:
            //    - enough PURE SPICE frames accumulated (spice_cnt >= 16)
            //    - no veto from PACKET (packet_cnt <= 2)
            //    - the "tail" after crumpling a large bag has passed (packet_cooldown == 0)
            else if (spice_cnt >= MIN_SPICE_FRAMES &&
                     packet_cnt <= MAX_PACKET_FRAMES_FOR_SPICE &&
                     packet_cooldown == 0U &&
                     sum_spice >= MIN_SCORE_SPICE &&
                     sum_spice > (sum_noise * NOISE_PENALTY_FACTOR))
            {
                g_final_detected_class = CLASS_SPICE;
            }
            else {
                g_final_detected_class = CLASS_SILENT;
            }
            LED_Set(g_final_detected_class == CLASS_SPICE);
        }
    }
}



/* ============================================================================
 * TASK 3: OLED Task
 * ============================================================================ */

void oled_tread(void *argument)
{
    (void)argument;

    I2C_LL_Init_test();

    ssd1306_Init();
    ssd1306_SetContrast(0x50);

    while (1)
    {
        char *class_text;

        // Read the final class after voting
        switch (g_final_detected_class)
        {
            case CLASS_SPICE:
                class_text = "SPICE";
                break;

            case CLASS_PACKET:
                class_text = "PACKET";
                break;

            case CLASS_SILENT:
            default:
                class_text = "NOISE";
                break;
        }

        // Clear the display buffer
        ssd1306_Fill(Black);

        // Center the string horizontally and vertically
        uint8_t text_width = strlen(class_text) * 11U;
        uint8_t x = (SSD1306_WIDTH - text_width) / 2U;
        uint8_t y = (SSD1306_HEIGHT - 18U) / 2U;

        ssd1306_SetCursor(x, y);

        // Output the class using 11x18 font
        ssd1306_WriteString(class_text, Font_11x18, White);

        // Transfer buffer contents to the OLED
        ssd1306_UpdateScreen();

        // Update period 1 second
        osDelay(200);
    }
}



/* ============================================================================
 * Initialization and startup
 * ============================================================================ */

void app_main(void)
{
    NVIC_SetPriorityGrouping(3);
    HSE_SetPLLI2S_16kHz();

    osKernelInitialize();

    // Microphone capture task
    const osThreadAttr_t mic_attr = {
        .name       = "mic_dma",
        .priority   = osPriorityHigh,
        .stack_size = 1024
    };

    //SVM model classificator 
    const osThreadAttr_t neai_attr = {
        .name       = "neai_ml",
        .priority   = osPriorityNormal,
        .stack_size = 2048
    };


    //OLED
     const osThreadAttr_t oled_attr = {
        .name       = "oled",
        .priority   = osPriorityBelowNormal,
        .stack_size = 512
    };

    osThreadNew(neai_thread, NULL, &neai_attr);
    osThreadNew(mic_thread,  NULL, &mic_attr);
    osThreadNew(oled_tread,  NULL, &oled_attr);

    osKernelStart();

    for (;;) {

    }
}