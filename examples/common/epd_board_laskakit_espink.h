/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Wiring of the LaskaKit ESPink-Shelf-2.9 (ESP32 + GDEY029T94).
 *
 * This is board knowledge, deliberately kept out of the driver component. In a
 * real application the same content belongs with the application or in a BSP
 * component that also owns the board's other peripherals.
 *
 * GPIO2 drives a MOSFET that switches the 3V3 rail feeding the display (and the
 * uSUP connector), so it has to be high before the panel answers, and can be
 * pulled low to save power between updates.
 */
#pragma once

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"

#define EPINK_SHELF_29_SPI_HOST     SPI2_HOST
#define EPINK_SHELF_29_PIN_MOSI     23
#define EPINK_SHELF_29_PIN_SCLK     18
#define EPINK_SHELF_29_PIN_CS       5
#define EPINK_SHELF_29_PIN_DC       17
#define EPINK_SHELF_29_PIN_RST      16
#define EPINK_SHELF_29_PIN_BUSY     4
#define EPINK_SHELF_29_PIN_POWER    2
#define EPINK_SHELF_29_PIXEL_CLK_HZ (10 * 1000 * 1000)

/** @brief Power hook for esp_lcd_epaper_config_t::power_ctrl. */
static inline esp_err_t epink_shelf_29_power_ctrl(bool on, void *user_ctx)
{
    (void)user_ctx;
    /* Claim the pin once: the driver calls this on every power transition, and
     * gpio_config() on an already configured pin makes the GPIO driver log a
     * conflict. */
    static bool configured;

    if (!configured) {
        const gpio_config_t cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << EPINK_SHELF_29_PIN_POWER,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), "espink", "power pin");
        configured = true;
    }
    return gpio_set_level(EPINK_SHELF_29_PIN_POWER, on ? 1 : 0);
}
