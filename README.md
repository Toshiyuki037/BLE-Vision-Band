<a id="readme-top"></a>

<!-- BADGES -->

<p align="center">
  <img src="https://img.shields.io/badge/Embedded-Systems-blue?style=for-the-badge" />
  <img src="https://img.shields.io/badge/BLE-nRF5340-green?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Vision-On--Device-orange?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Haptics-DRV2605L-purple?style=for-the-badge" />
</p>

<br />

<!-- TITLE -->

<div align="center">
  <h1>WBLE Vision Band</h1>
  <p><strong>Embedded BLE Vision System with Haptic Feedback</strong></p>

  <p>
    A wrist-mounted system that performs on-device image recognition and communicates results through real-time haptic feedback.
  </p>

  <br />

<a href="#">🎥 View Demo (Coming Soon)</a>

</div>

---

## 📌 Overview

Vision-Band is a wearable embedded system designed to explore on-device perception in resource-constrained environments.

Motivated by the limitations of the Even Realities G2 Frames—specifically the lack of an integrated camera—this project implements a modular vision pipeline paired with a haptic feedback interface for real-time, non-visual interaction.

---

## 🧠 System Architecture

```bash
[Camera] → [nRF5340 MCU] → [Processing / ML] → [DRV2605L] → [Vibration Motor]
                               ↓
                             [BLE]
```

* SPI camera streams image data to MCU
* MCU performs lightweight processing / inference
* Haptic driver converts results into vibration feedback
* BLE enables debugging + external communication

---

## 🔧 Hardware Components

* NRF5340-DK — main MCU + BLE
* Arducam Mega 5MP SPI Camera — image capture
* DRV2605L — haptic driver
* 3V vibration motor — feedback output
* Li-ion battery (500mAh) — power
* TP4056 — charging module

---

## 📦 Bill of Materials (BOM)

A detailed breakdown of all components, costs, and sourcing is available below:

👉 [View BOM (Google Sheet)](https://docs.google.com/spreadsheets/d/1EQedtdF9KUZCxSDgVT32OZnp6HhoeuN5VWY41QO01Ro/edit?usp=sharing)

* Includes part numbers, suppliers, and prototyping cost (~$187)
* Documents full hardware stack for reproducibility

---

## ⚙️ Software Stack

* Embedded firmware (C/C++)
* SPI image acquisition pipeline
* I²C haptic control (DRV2605L)
* BLE communication (Nordic SDK)
* Lightweight image recognition (TinyML / rule-based)

---

## 🚀 Features

* On-device image capture and processing
* Real-time haptic feedback output
* BLE-enabled communication interface
* Fully self-contained wearable prototype
* End-to-end embedded system (hardware + firmware integration)

---

## 📊 Challenges & Engineering Tradeoffs

* **Memory constraints:** Limited RAM for image buffering
* **Bandwidth limits:** SPI camera throughput bottlenecks
* **Power system limitations:** TP4056 lacks load sharing
* **Compute vs latency:** Balancing ML accuracy and responsiveness

---

## 🔋 Power Considerations

* Powered by 3.7V Li-ion battery (500mAh)
* Charging via TP4056
* System currently optimized for prototyping, not all-day wearable use

---

## 🛠️ Getting Started

### Prerequisites

* Arduino IDE or Nordic SDK
* USB connection for NRF5340-DK
* Basic embedded development setup

### Installation

```bash
git clone https://github.com/Toshiyuki037/BLE-Vision-Band.git
```

1. Open firmware in IDE
2. Connect hardware components
3. Flash firmware to board
4. Power via battery + charging module

---

## 📁 Project Structure

```bash
/firmware      → Embedded code  
/hardware      → Schematics / BOM  
/docs          → Design notes  
```

---

## 🗺️ Roadmap

* [x] Hardware integration
* [x] Camera + MCU interface
* [ ] Image recognition optimization
* [ ] Power system redesign
* [ ] Custom PCB
* [ ] Wearable enclosure

---

## 📸 Demo

> Coming soon — will include real-time detection + haptic feedback demo

---

## 📜 License

Distributed under the MIT License.

---

## 📬 Contact

Your Name
GitHub: https://github.com/Toshiyuki037

Project Link:
https://github.com/Toshiyuki037/BLE-Vision-Band

---

<p align="center">↑ Back to top</p>
