# ESP32 Web Radio with WebUI and Android App

This project transforms the **ESP32** into a powerful **Internet Radio** with a responsive Web Interface and optional Android app control.

## 🚀 Overview

Features:
- 🎵 **Internet Radio Streaming** (MP3)
- 🌐 **Responsive Web Interface** for:
  - Station selection
  - Volume control
  - Streaming status
- 📲 **Android App** API compatibility
- 🖥️ **SSD1306 OLED Display**:
  - Station name
  - Metadata (stream title)
  - Wi-Fi status and signal strength
- 🔊 **PCM5102A I2S DAC** for high-quality audio output
- ⚡ **Battery Powered** with LX-2B UPS module and 18650 batteries

---

## 🛠 Hardware Used

| Component                       | Description                                  |
|---------------------------------|----------------------------------------------|
| **ESP32-D0WD-V3 (rev 3.1)**     | Dual Core, 240 MHz Wi-Fi + Bluetooth SoC    |
| **SSD1306 OLED 128x64**         | Display station info and status             |
| **PCM5102A DAC**                | High-fidelity I2S audio output              |
| **LX-2B UPS Module**            | UPS power with 2×18650 batteries in parallel|
| **Power Source**                | 2×18650 Lithium batteries                   |
