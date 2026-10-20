# ESP32-S3 Bore DRO (Digital Readout for Boring Head)

A precision Digital Readout (DRO) designed for machinist boring heads, turning tools, and radius-to-diameter measurements. Built on the **LILYGO T-Display-S3** (ESP32-S3) reading directly from standard 24-bit digital calipers over USB/data wires.

---

## 🛠️ Features

- **True Radial-to-Diameter Calculation ($2\times$):** Measures the radial cutter extension and doubles the movement so you get direct final bored diameter readouts.
- **Instant Base Presets:** Switch between pre-configured reference diameters (**28.0 mm**, **54.0 mm**, **82.0 mm**) with a single short press.
- **One-Button Calibration:** Long press (>1s) zeros and stores the offset into non-volatile Flash memory (`Preferences`). Offsets survive power cycles and reboots.
- **Flicker-Free 16-bit Double Buffered UI:** Uses `TFT_eSprite` (320×170) for seamless 60 FPS updates on the ST7789 IPS display.
- **Rock-Solid Caliper Protocol Decoding:** High-impedance inputs sampled on the `RISING` clock edge eliminate electrical noise spikes and number jumping.
- **Smart Battery & Power Management:**
  - Real-time Li-ion battery voltage sensing via onboard ADC divider.
  - Animated charging battery gauge while plugged into USB.
  - Automatic 30-second inactivity deep sleep when running on battery (sleep timer is disabled while plugged into USB power).
  - Wake-up via onboard button interrupt or hardware reset.
- **Industrial Startup Screen:** 5.5-second boot sequence with an animated progress loading bar.

---

## 🔌 Hardware & Wiring

### Components
- **Microcontroller & Display:** [LILYGO T-Display-S3](https://github.com/Xinyuan-LilyGO/T-Display-S3) (ESP32-S3, 16MB Flash, 8MB OPI PSRAM, 1.9" 170×320 ST7789 IPS Display).
- **Caliper:** Standard digital caliper with 24-bit digital serial data port (CR2032 3V coin cell type).
- **Battery:** 3.7V Li-ion battery (e.g., PS4 controller battery ~1000mAh) via onboard 2-pin JST connector.
- **Button:** Onboard button on **GPIO 14** (active LOW) for preset switching and calibration.

### Caliper Connection (USB Cable Mapping)
| USB Wire Color | Caliper Signal | LILYGO T-Display-S3 Pin | Notes |
| :--- | :--- | :--- | :--- |
| ⬜ **White** (`D-`) | **Clock (CLK)** | **GPIO 1** | Configured as high-impedance `INPUT` |
| 🟩 **Green** (`D+`) | **Data (DATA)** | **GPIO 2** | Configured as high-impedance `INPUT` |
| 🟨 **Yellow / Black** | **Ground (GND)**| **GND** | Common ground with ESP32 |
| 🟥 **Red** (`VBUS`) | **Power (3V3)** | *(Optional)* | Only connect to 3V3 if caliper coin cell is **removed**! |

> [!CAUTION]
> If your caliper has its own 3V coin cell battery installed, **do NOT connect the 3V3 red wire**! Powering a non-rechargeable coin cell externally will cause ground noise and signal jitter. Only connect **CLK, DATA, and GND**.

---

## ⚙️ Arduino IDE Configuration

1. **Board Manager:** Install `esp32` by Espressif Systems.
2. **Board:** `ESP32S3 Dev Module`
3. **Settings:**
   - **Flash Size:** `16MB (128Mb)`
   - **PSRAM:** `OPI PSRAM`
   - **USB CDC On Boot:** `Enabled`
   - **Upload Mode:** `UART0 / Hardware CDC`
4. **Library:** `TFT_eSPI` by Bodmer
   - In Arduino libraries folder: open `TFT_eSPI/User_Setup_Select.h`
   - Comment out `#include <User_Setup.h>`
   - Uncomment `#include <User_Setups/Setup206_LilyGo_T_Display_S3.h>`

---

## 🕹️ User Controls

- **Short Press (< 1s):** Cycles the active base target (`28.0 mm` $\rightarrow$ `54.0 mm` $\rightarrow$ `82.0 mm`) and saves selection to flash.
- **Long Press (> 1s):** Calibrates the zero position against the active base size. Screen flashes **`CALIBRATED!`** banner for 1.5 seconds.
- **Deep Sleep:** Unit automatically goes into low-power deep sleep 30 seconds after the caliper stops transmitting (when on battery).
- **Wake:** Press the onboard GPIO 14 button or hardware RST button.
