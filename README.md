# Real-Time Acoustic Event Classification on STM32F4 (SVM + RTX5)

A real-time embedded TinyML project running on the **STM32F401RCT6**. It captures audio from an **INMP441** I2S MEMS microphone, classifies acoustic events using a lightweight **SVM model**, and displays the results on an **SSD1306 OLED**. The application runs under **CMSIS-RTOS2 (RTX5)**.

**Memory footprint:** 18.3 KB Code · 4.5 KB RO-data · 24.2 KB RAM  
<sub>Arm Compiler 6 · Build output</sub>

---

## Demo Video


[![Watch the demo](https://img.youtube.com/vi/Wq74rr61Sos/hqdefault.jpg)](https://www.youtube.com/shorts/Wq74rr61Sos)

---

## Demo Behavior & Output Logic

Although the underlying **SVM model is trained on 4 distinct acoustic classes** (`SPICE`, `PACKET`, `SILENT`, `NOISE`), the user interface maps them into **3 continuous display states**, ensuring one active status is always shown on the OLED screen:

| Internal SVM Class | Physical Sound | OLED Display (`SSD1306`) | Status LED (`PC13`) |
| :--- | :--- | :---: | :---: |
| **`SPICE`** | Rustling of a small spice sachet (dense/foil-paper) | **`SPICE`** | **ON** |
| **`PACKET`** | Rustling of a large plastic bag (polyethylene) | **`PACKET`** | OFF |
| **`SILENT`** | Quiet room / idle state | **`NOISE`** | OFF |
| **`NOISE`** | Ambient background noise / speech / knocks | **`NOISE`** | OFF |

> Both `SILENT` and `NOISE` represent the idle/non-target background state on the display, while the onboard LED (`PC13`) acts as a dedicated hardware indicator specifically for `SPICE` detection.

---

## Toolchain & Environment

* **Environment:** VS Code
* **Toolchain:** Arm Compiler 6.24 (Keil MDK v6)
* **Build System:** CMSIS-Toolbox (`csolution` / `cbuild`)
* **Target MCU:** STM32F401RCT6 (LQFP64, 256 KB Flash, 64 KB RAM, hardware FPU ON)
* **RTOS:** RTX5 (CMSIS-RTOS2)
* **Audio Sensor:** INMP441 (24-bit I2S MEMS Microphone, configured at 16 kHz mono)
* **Display:** SSD1306 OLED (I2C, `11x18` bitmap font)
* **Indicator:** Onboard LED (`PC13`, active-low)

---

## Hardware Pinout

* **I2S Microphone (`INMP441` via `SPI2` Master RX + `DMA1 Stream 3` Double-Buffer Mode):**
  * `PB12` — `I2S2_WS` (Word Select / LRCK)
  * `PB13` — `I2S2_CK` (Bit Clock)
  * `PB15` — `I2S2_SD` (Serial Data)
  * `PB3` — `L/R` Channel Select (driven `LOW` via GPIO to select Left channel)
* **OLED Display (`SSD1306` via `I2C`):**
  * `PB8` — `I2C_SCL` (Clock)
  * `PB9` — `I2C_SDA` (Data)
* **Status LED:**
  * `PC13` — Push-Pull output, active-low (controlled via `GPIOC->BSRR`)

---

## Signal Processing & ML Pipeline

The firmware reproduces the exact digital signal conditioning pipeline used during dataset acquisition so the runtime features match the training distribution:

### 1. Real-Time Audio Front-End
* **Sample Rate:** `16 kHz` (sourced via `PLLI2S`).
* **Zero-Loss DMA Capture:** Double-buffered circular DMA (`buffer_dma_A` and `buffer_dma_B`), each delivering `256` mono samples (`16 ms`) per interrupt.
* **DC-Blocking Filter:** Exponential high-pass filter (`MIC_DC_SHIFT = 7`, $f_c \approx 20\text{ Hz}$) to remove microphone DC offset.
* **Fixed-Point Gain & Saturation:** `14.0x` gain (`Q8` fixed-point math) with hardware 16-bit saturation (`__SSAT`), converted to single-precision `float` via Cortex-M4 FPU.

### 2. Sliding Window with 50% Overlap
* **Window Size:** `1024` samples (**64 ms**).
* **Step Size (50% Overlap):** `512` samples (**32 ms**).
* Every **32 ms** (~31.25 times per second), the buffer shifts by half a window and triggers a new ML inference pass.

### **3. Lightweight SVM Model**

Instead of heavy neural networks (CNN/LSTM), the project uses a compact **time-domain Linear SVM** (One-vs-Rest across 4 classes):

1. **Detrending:** Mean subtraction across the 1024-sample window.
2. **Feature Extraction:** Extracts 6 statistical and time-domain features:
   - `crest_factor` (impulse sharpness)
   - `kurtosis` (peakedness of amplitude distribution)
   - `entropy` (signal randomness across 20 histogram bins)
   - `rms` (signal energy)
   - `num_zero_crossings` (dominant frequency proxy)
   - `std` (amplitude dispersion)
3. **Robust Normalization:** Quantile-based scaling (\(Q_{0.25}\) to \(Q_{0.89}\)).
4. **SVM Inference:** Linear dot products + probability calibration, outputting class confidence scores in `[0.0 .. 1.0]`.
5. **Inference Performance:** Full pipeline execution in just **1.2 ms** on an **STM32F401 (ARM Cortex-M4F @ 84 MHz)**.

### 4. Temporal Voting & Anti-False-Trigger Logic
Because short 64 ms windows can catch transient micro-pauses or onset/release crackles of a large plastic bag, raw SVM predictions pass through a **1-second sliding ring buffer** (`32` steps × `32 ms` = `1.024 s`):
* **Separate Confidence Thresholds:** Requires single-window probability $\ge 0.80$ for `SPICE` and $\ge 0.75$ for `PACKET`.
* **Minimum Persistence:** Requires at least `14` active frames for `SPICE` and `10` for `PACKET` within the 1-second window, ignoring isolated knocks or clicks.
* **Noise Penalty:** Mid-confidence ambiguous frames (`0.35 < p_spice < 0.80`) and `NOISE` frames penalize target scores (`1.5x` penalty factor).
* **Plastic Bag Veto & Tail Cooldown:** Prevents `SPICE` false triggers during the attack/release crackles of a plastic bag (`PACKET`) by enforcing a veto (`max 2 PACKET frames` allowed) and a `25-step` (~0.8 s) cooldown timer after `PACKET` activity ends.

---

## RTOS Architecture (RTX5)

The application is split into three decoupled threads synchronized via RTOS Thread Flags:

1. **Audio Capture Task (`osPriorityHigh`, Stack: `1024 B`):**
   Woken by DMA Transfer Complete interrupts (`FLAG_BUF_A` / `FLAG_BUF_B`), runs DC filtering and gain, assembles the 50%-overlapping 1024-sample window, and signals the classifier task.
2. **SVM Classification & Voting Task (`osPriorityNormal`, Stack: `2048 B`):**
   Executes feature extraction, SVM inference (profiled in microseconds via the Cortex-M4 `DWT->CYCCNT` cycle counter), updates the 1-second voting ring buffer, and drives `LED_Set()` on `PC13`.
3. **OLED Display Task (`osPriorityBelowNormal`, Stack: `512 B`):**
   Refreshes the `SSD1306` screen every `200 ms` with the current stabilized state (`SPICE`, `PACKET`, or `NOISE`).

---

## Memory Footprint & Efficiency

A key advantage of pairing time-domain feature extraction with a **Linear SVM** over deep learning alternatives is its ultra-low Flash and RAM footprint—leaving plenty of headroom on the **STM32F401RCT6** (256 KB Flash / 64 KB RAM).

### Build Output Summary (Arm Compiler 6)
```text
Code = 18268   RO-data = 4492   RW-data = 172   ZI-data = 24812
```

The `Font_11x18` OLED bitmap font occupies approximately **3,420 bytes (3.34 KB)** of read-only Flash memory.