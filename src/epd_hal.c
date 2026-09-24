/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Transport and GPIO helpers shared by all controller drivers.
 */
#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "epd_private.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epaper";

esp_err_t epd_tx_cmd(epd_dev_t *dev, uint8_t cmd, const uint8_t *params, size_t len)
{
    return esp_lcd_panel_io_tx_param(dev->io, cmd, params, len);
}

esp_err_t epd_tx_data(epd_dev_t *dev, uint8_t cmd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    /* The first chunk carries the command byte, the rest continues the stream
     * with the controller's auto-incrementing RAM pointer. */
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > EPD_TX_CHUNK) {
            chunk = EPD_TX_CHUNK;
        }
        const uint8_t *src = p + sent;
        if (dev->bounce) {
            /* The framebuffer lives in memory the SPI driver cannot DMA from,
             * so stage the chunk. A handful of 4 KB copies is nothing against
             * a refresh measured in seconds. */
            memcpy(dev->bounce, src, chunk);
            src = dev->bounce;
        }
        esp_err_t err = (sent == 0) ? esp_lcd_panel_io_tx_color(dev->io, cmd, src, chunk)
                                    : esp_lcd_panel_io_tx_color(dev->io, -1, src, chunk);
        if (err != ESP_OK) {
            return err;
        }
        sent += chunk;
    }
    return ESP_OK;
}

bool epd_busy_asserted(const epd_dev_t *dev)
{
    if (dev->busy_gpio < 0) {
        /* Nothing to read: the caller has to assume the panel is still working
         * and fall back to the worst-case delay. */
        return true;
    }
    return gpio_get_level(dev->busy_gpio) == (dev->desc->busy_active_high ? 1 : 0);
}

esp_err_t epd_wait_busy(epd_dev_t *dev, uint32_t busy_ms, uint32_t timeout_ms)
{
    if (dev->busy_gpio < 0) {
        /* No BUSY line: sleep the worst case. busy_ms and not timeout_ms,
         * whose headroom would be dead time on every wait. */
        vTaskDelay(pdMS_TO_TICKS(busy_ms));
        return ESP_OK;
    }

    const int busy_level = dev->desc->busy_active_high ? 1 : 0;

    /* Let the panel raise BUSY first. An earlier poll reads "idle" and the
     * caller then writes RAM mid-waveform. */
    vTaskDelay(pdMS_TO_TICKS(1));

    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline = start_us + (int64_t)timeout_ms * 1000;

    /* Short spin first: a partial update can release BUSY inside one tick. */
    for (int i = 0; i < 20; i++) {
        if (gpio_get_level(dev->busy_gpio) != busy_level) {
            ESP_LOGD(TAG, "BUSY held <2 ms (allowed %" PRIu32 ")", busy_ms);
            return ESP_OK;
        }
        esp_rom_delay_us(100);
    }
    while (gpio_get_level(dev->busy_gpio) == busy_level) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "BUSY stuck for %" PRIu32 " ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* The real duration: what a descriptor's worst case must cover, and what
     * a board without BUSY sleeps through. */
    ESP_LOGD(TAG, "BUSY held %lld ms (allowed %" PRIu32 ")", (esp_timer_get_time() - start_us) / 1000, busy_ms);
    return ESP_OK;
}

void epd_hw_reset(epd_dev_t *dev)
{
    if (dev->reset_gpio < 0) {
        return;
    }
    gpio_set_level(dev->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(dev->desc->reset_high_ms));
    gpio_set_level(dev->reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(dev->desc->reset_low_ms));
    gpio_set_level(dev->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(dev->desc->reset_high_ms));
}
