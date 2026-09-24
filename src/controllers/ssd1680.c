/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Solomon Systech SSD1680 / SSD1681 family controller driver.
 *
 * Partial updates use the controller's built-in display mode 2 waveform
 * (no custom LUT upload): the controller compares the "previous" RAM (0x26)
 * with the current RAM (0x24) and only drives the pixels that differ. The
 * front-end keeps both RAMs in sync after every refresh.
 */
#include <string.h>

#include "epd_private.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1680_cmd.h"

static const char *TAG = "ssd1680";

static esp_err_t ssd1680_power_off(epd_dev_t *dev)
{
    if (!dev->power_on) {
        return ESP_OK;
    }
    const uint8_t seq = SSD1680_POWER_OFF;

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DISPLAY_UPDATE_CTRL2, &seq, 1), TAG, "power off");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_MASTER_ACTIVATION, NULL, 0), TAG, "activate");
    ESP_RETURN_ON_ERROR(epd_wait_busy(dev, SSD1680_POWER_OFF_MS, 500), TAG, "busy");
    dev->power_on = false;
    return ESP_OK;
}

static esp_err_t ssd1680_set_window(epd_dev_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    const uint16_t x_end = x + w - 1;
    const uint16_t y_end = y + h - 1;
    const uint8_t entry_mode = 0x03; /* X+, Y+ */
    const uint8_t x_range[] = { x / 8, x_end / 8 };
    const uint8_t y_range[] = { y & 0xFF, y >> 8, y_end & 0xFF, y_end >> 8 };
    const uint8_t x_counter = x / 8;
    const uint8_t y_counter[] = { y & 0xFF, y >> 8 };

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DATA_ENTRY_MODE, &entry_mode, 1), TAG, "entry mode");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_RAM_X_RANGE, x_range, sizeof(x_range)), TAG, "x range");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_RAM_Y_RANGE, y_range, sizeof(y_range)), TAG, "y range");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_RAM_X_COUNTER, &x_counter, 1), TAG, "x counter");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_RAM_Y_COUNTER, y_counter, sizeof(y_counter)), TAG, "y counter");
    return ESP_OK;
}

static esp_err_t ssd1680_init(epd_dev_t *dev)
{
    const epd_panel_desc_t *desc = dev->desc;
    const uint16_t mux = desc->gate_lines - 1;
    const uint8_t driver_output[] = { mux & 0xFF, mux >> 8, 0x00 };
    const uint8_t entry_mode = 0x03;

    if (dev->hibernating || !dev->init_done) {
        epd_hw_reset(dev);
    }
    ESP_RETURN_ON_ERROR(epd_wait_busy(dev, SSD1680_RESET_SETTLE_MS, 1000), TAG, "busy after reset");

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_SW_RESET, NULL, 0), TAG, "sw reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(epd_wait_busy(dev, SSD1680_RESET_SETTLE_MS, 1000), TAG, "busy after sw reset");

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DRIVER_OUTPUT_CTRL, driver_output, sizeof(driver_output)),
                        TAG,
                        "driver output");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DATA_ENTRY_MODE, &entry_mode, 1), TAG, "entry mode");

    /* Everything that differs between panels on this controller - border
     * waveform, source range, temperature sensor, supply voltages - comes from
     * the descriptor, so a new panel is a table and not a change in here. */
    for (size_t i = 0; i < desc->init_cmd_count; i++) {
        const epd_init_cmd_t *c = &desc->init_cmds[i];
        ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, c->cmd, c->data, c->len), TAG, "init cmd 0x%02x", c->cmd);
    }
    ESP_RETURN_ON_ERROR(ssd1680_set_window(dev, 0, 0, desc->width, desc->height), TAG, "window");

    dev->hibernating = false;
    dev->power_on = false;
    return ESP_OK;
}

static esp_err_t ssd1680_write_ram(epd_dev_t *dev, epd_ram_t ram, const uint8_t *data, size_t len)
{
    const uint8_t cmd = (ram == EPD_RAM_PREVIOUS) ? SSD1680_CMD_WRITE_RAM_RED : SSD1680_CMD_WRITE_RAM_BW;

    return epd_tx_data(dev, cmd, data, len);
}

static esp_err_t ssd1680_refresh_start(epd_dev_t *dev, bool partial)
{
    const uint8_t seq = partial ? SSD1680_UPDATE_PARTIAL : SSD1680_UPDATE_FULL;

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DISPLAY_UPDATE_CTRL2, &seq, 1), TAG, "update ctrl 2");
    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_MASTER_ACTIVATION, NULL, 0), TAG, "activate");

    /* Describes where the sequence leaves the panel once it finishes: the full
     * waveform powers it down on its own, the partial one leaves the charge
     * pump running for the next update. Nothing reads this before the caller
     * has waited for BUSY, so recording it up front is safe. */
    dev->power_on = partial;
    return ESP_OK;
}

static esp_err_t ssd1680_sleep(epd_dev_t *dev)
{
    ESP_RETURN_ON_ERROR(ssd1680_power_off(dev), TAG, "power off");
    if (dev->reset_gpio < 0) {
        /* Without RST there is no way back out of deep sleep. */
        ESP_LOGW(TAG, "no RST pin, skipping deep sleep");
        return ESP_OK;
    }
    const uint8_t mode = 0x01;

    ESP_RETURN_ON_ERROR(epd_tx_cmd(dev, SSD1680_CMD_DEEP_SLEEP, &mode, 1), TAG, "deep sleep");
    dev->hibernating = true;
    dev->init_done = false;
    return ESP_OK;
}

const struct epd_controller_t epd_controller_ssd1680 = {
    .name = "ssd1680",
    .init = ssd1680_init,
    .set_window = ssd1680_set_window,
    .write_ram = ssd1680_write_ram,
    .refresh_start = ssd1680_refresh_start,
    .power_off = ssd1680_power_off,
    .sleep = ssd1680_sleep,
};
