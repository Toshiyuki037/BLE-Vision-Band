# BLE-Vision-Band
Wrist-mounted embedded vision system using on-device image recognition (nRF5340 + SPI camera) with BLE connectivity and real-time haptic feedback.

**Motivation**
Built to overcome the lack of an integrated camera in the Even Realities G2 Frames, this project implements a wrist-mounted embedded vision system to explore real-time perception and haptic feedback in wearable devices.

**System Architecture**
The system integrates a SPI camera module with a Nordic nRF5340 MCU for on-device processing. Results are transmitted over BLE and conveyed via a haptic driver controlling a vibration motor.

**BOM**
https://docs.google.com/spreadsheets/d/1EQedtdF9KUZCxSDgVT32OZnp6HhoeuN5VWY41QO01Ro/edit?usp=sharing

**Software**
Embedded firmware (C/C++)
SPI camera interface + image buffering
Lightweight image recognition (TinyML or rule-based)
I²C haptic control (DRV2605L)
BLE communication stack

**Features**
On-device image capture and processing
Real-time haptic feedback output
BLE connectivity for external interfacing
Fully self-contained wearable prototype
