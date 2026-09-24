/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define SSD1680_CMD_DRIVER_OUTPUT_CTRL   0x01
#define SSD1680_CMD_GATE_VOLTAGE         0x03
#define SSD1680_CMD_SOURCE_VOLTAGE       0x04
#define SSD1680_CMD_DEEP_SLEEP           0x10
#define SSD1680_CMD_DATA_ENTRY_MODE      0x11
#define SSD1680_CMD_SW_RESET             0x12
#define SSD1680_CMD_TEMP_SENSOR_CTRL     0x18
#define SSD1680_CMD_MASTER_ACTIVATION    0x20
#define SSD1680_CMD_DISPLAY_UPDATE_CTRL1 0x21
#define SSD1680_CMD_DISPLAY_UPDATE_CTRL2 0x22
#define SSD1680_CMD_WRITE_RAM_BW         0x24
#define SSD1680_CMD_WRITE_RAM_RED        0x26 /* "previous" image for differential update */
#define SSD1680_CMD_WRITE_VCOM           0x2C
#define SSD1680_CMD_WRITE_LUT            0x32
#define SSD1680_CMD_BORDER_WAVEFORM      0x3C
#define SSD1680_CMD_RAM_X_RANGE          0x44
#define SSD1680_CMD_RAM_Y_RANGE          0x45
#define SSD1680_CMD_RAM_X_COUNTER        0x4E
#define SSD1680_CMD_RAM_Y_COUNTER        0x4F

/* Display update control 2 sequences */
#define SSD1680_UPDATE_FULL              0xF7 /* load OTP LUT1, full flashing waveform */
#define SSD1680_UPDATE_PARTIAL           0xFC /* display mode 2, fast differential waveform */
#define SSD1680_POWER_ON                 0xE0
#define SSD1680_POWER_OFF                0x83

/* Blind delays for boards that do not wire BUSY. The 10 ms reset settling is
 * from the SSD1680 datasheet; it measures under 2 ms on a GDEY029T94.
 * Power-off is not in the datasheet: 137 ms measured, rounded up. Debug
 * logging prints the real durations. */
#define SSD1680_RESET_SETTLE_MS          10
#define SSD1680_POWER_OFF_MS             150
