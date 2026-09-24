/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Internal contract between the esp_lcd front-end (esp_lcd_epaper.c), the
 * controller drivers (src/controllers/) and the panel descriptors
 * (src/panels/).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_epaper.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct epd_dev_t epd_dev_t;

/** @brief Which controller RAM a buffer write targets. */
typedef enum {
    EPD_RAM_CURRENT = 0, /*!< The image to be shown by the next refresh */
    EPD_RAM_PREVIOUS,    /*!< The image the controller assumes is on the glass (differential update) */
} epd_ram_t;

/**
 * @brief Operations every controller driver implements.
 *
 * All coordinates are in native panel orientation; `x` and `w` are already
 * byte-aligned by the caller.
 */
struct epd_controller_t {
    const char *name;
    /** Power-on / soft-reset sequence, leaves the controller ready for RAM writes. */
    esp_err_t (*init)(epd_dev_t *dev);
    /** Select the RAM rectangle the following write_ram() calls fill. */
    esp_err_t (*set_window)(epd_dev_t *dev, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    /** Stream pixel bytes into the selected RAM. */
    esp_err_t (*write_ram)(epd_dev_t *dev, epd_ram_t ram, const uint8_t *data, size_t len);
    /** Trigger the waveform and return; the front-end waits for BUSY to clear. */
    esp_err_t (*refresh_start)(epd_dev_t *dev, bool partial);
    /** Turn the charge pump off, keeping the controller addressable. */
    esp_err_t (*power_off)(epd_dev_t *dev);
    /** Enter deep sleep; only a hardware reset brings the controller back. */
    esp_err_t (*sleep)(epd_dev_t *dev);
};

/** @brief Runtime state of one panel instance. */
struct epd_dev_t {
    esp_lcd_panel_t base; /*!< Must stay first, the esp_lcd handle points here */
    esp_lcd_panel_io_handle_t io;
    const epd_panel_desc_t *desc;
    int reset_gpio;
    int busy_gpio;
    epd_power_ctrl_cb_t power_ctrl;
    void *power_ctrl_ctx;
    uint8_t *fb;      /*!< Current image, native orientation, 1bpp, 1 = white */
    uint8_t *fb_prev; /*!< Image the glass is assumed to show, CPU-only, never sent */
    uint8_t *bounce;  /*!< DMA-capable staging buffer, NULL when `fb` is itself DMA-capable */
    size_t fb_size;
    uint16_t stride; /*!< Bytes per native row, (width + 7) / 8 */
    epd_rotation_t rotation;
    bool mirror_x;
    bool mirror_y;
    bool invert;
    int gap_x;
    int gap_y;
    epd_refresh_mode_t mode;
    uint16_t full_refresh_interval;
    uint16_t partial_count;
    bool auto_refresh;
    bool init_done;
    bool hibernating;
    bool power_on;
    bool prev_valid;          /*!< false until a full refresh has synced both RAMs */
    bool refresh_pending;     /*!< A waveform is running and has not been waited on yet */
    bool refresh_partial;     /*!< Which waveform that is */
    int64_t refresh_start_us; /*!< When it was started, for panels with no BUSY pin */
    uint16_t dirty_x;         /*!< Region a pending partial refresh is driving */
    uint16_t dirty_y;
    uint16_t dirty_w;
    uint16_t dirty_h;
};

/**
 * Transfers are split into chunks of this size. esp_lcd_panel_io_tx_color()
 * would split a large payload itself, but a bounded chunk keeps the DMA
 * descriptor list short and gives the bounce buffer a fixed size.
 */
#define EPD_TX_CHUNK 4000

/* Helpers shared by the controller drivers, implemented in epd_hal.c. */

/** @brief Send a command byte followed by optional parameter bytes. */
esp_err_t epd_tx_cmd(epd_dev_t *dev, uint8_t cmd, const uint8_t *params, size_t len);
/** @brief Send a command byte followed by a (potentially large) pixel payload. */
esp_err_t epd_tx_data(epd_dev_t *dev, uint8_t cmd, const void *data, size_t len);
/**
 * @brief Wait for the panel to finish an operation.
 *
 * The two are not interchangeable: `busy_ms` is slept through when there is no
 * BUSY pin, `timeout_ms` only bounds the polling when there is one.
 *
 * @param[in] dev        Panel instance
 * @param[in] busy_ms    Worst-case duration of the operation
 * @param[in] timeout_ms How long to poll BUSY before returning ESP_ERR_TIMEOUT
 */
esp_err_t epd_wait_busy(epd_dev_t *dev, uint32_t busy_ms, uint32_t timeout_ms);
/** @brief Whether BUSY is asserted right now; true when there is no BUSY pin to read. */
bool epd_busy_asserted(const epd_dev_t *dev);
/** @brief Pulse the RST line according to the panel timings. */
void epd_hw_reset(epd_dev_t *dev);

#ifdef __cplusplus
}
#endif
