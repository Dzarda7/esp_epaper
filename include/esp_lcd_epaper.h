/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * esp_epaper - a generic e-paper panel driver for ESP-IDF, exposed through the
 * standard esp_lcd panel interface.
 *
 * The panel handle returned by esp_lcd_new_panel_epaper() is a regular
 * esp_lcd_panel_handle_t: esp_lcd_panel_draw_bitmap(), esp_lcd_panel_mirror(),
 * esp_lcd_panel_swap_xy() and LVGL's esp_lvgl_port all work unmodified.
 *
 * E-paper specifics that the esp_lcd model has no concept of - full vs partial
 * waveform, deep sleep, panel power rail - are exposed as the epaper_panel_*
 * extension calls below.
 *
 * Drawing is buffered: draw_bitmap() composes into an internal 1bpp
 * framebuffer, and the glass is only updated when epaper_panel_refresh() is
 * called (or automatically, see esp_lcd_epaper_config_t.auto_refresh).
 *
 * Threading: a panel handle is not thread safe. Drawing and refreshing from
 * more than one task needs external serialisation.
 *
 * Timing: an update takes seconds - about 3 s full, 0.5 s partial - because the
 * waveform runs on the panel, not the CPU. epaper_panel_refresh() blocks for
 * all of it; epaper_panel_refresh_start() and epaper_panel_refresh_wait() split
 * it so the caller can do something else meanwhile.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_panels.h"
#include "esp_err.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Display orientation, applied on top of the panel's native portrait layout. */
typedef enum {
    EPD_ROTATION_0 = 0, /*!< Native portrait, 128x296 for a 2.9" panel */
    EPD_ROTATION_90,    /*!< Landscape, 296x128 */
    EPD_ROTATION_180,   /*!< Portrait upside down */
    EPD_ROTATION_270,   /*!< Landscape, the other way up */
} epd_rotation_t;

/**
 * @brief Hook the driver calls to switch the panel's power rail.
 *
 * Whether the rail is a GPIO driving a MOSFET, an LDO enable or a PMIC register
 * is board knowledge, so the driver only asks - it does not touch any pin. The
 * callback must return once the rail is stable.
 *
 * @param[in] on       true to power the panel up, false to cut it
 * @param[in] user_ctx Context from esp_lcd_epaper_config_t.power_ctrl_ctx
 */
typedef esp_err_t (*epd_power_ctrl_cb_t)(bool on, void *user_ctx);

/** @brief Waveform used by epaper_panel_refresh(). */
typedef enum {
    EPD_REFRESH_AUTO = 0, /*!< Partial when possible, full every `full_refresh_interval` updates */
    EPD_REFRESH_FULL,     /*!< Slow (~3 s), flashing, clears ghosting */
    EPD_REFRESH_PARTIAL,  /*!< Fast (~0.5 s), no flash, accumulates ghosting */
} epd_refresh_mode_t;

/**
 * @brief Vendor configuration of an e-paper panel.
 *
 * Assign a pointer to this struct to esp_lcd_panel_dev_config_t.vendor_config.
 */
typedef struct {
    const epd_panel_desc_t *panel;   /*!< Panel descriptor, e.g. &epd_panel_gdey029t94 (required) */
    int busy_gpio_num;               /*!< GPIO of the BUSY pin, -1 to poll blindly with worst-case delays */
    epd_power_ctrl_cb_t power_ctrl;  /*!< Optional hook switching the panel power rail, NULL if always powered */
    void *power_ctrl_ctx;            /*!< Passed to `power_ctrl` */
    epd_rotation_t rotation;         /*!< Orientation at init, changeable later with epaper_panel_set_rotation() */
    epd_refresh_mode_t refresh_mode; /*!< Waveform selection policy, default EPD_REFRESH_AUTO */
    uint16_t full_refresh_interval;  /*!< In AUTO mode, force a full refresh every N updates (0 = only on demand) */
    bool auto_refresh;               /*!< Refresh the glass at the end of every draw_bitmap() call */
    struct {
        /**
         * Prefer PSRAM for the framebuffers, for panels whose two buffers no
         * longer fit internal RAM. Transfers then stage through a 4 KB
         * bounce buffer: one memcpy per chunk, against a refresh of seconds.
         */
        uint32_t fb_in_psram : 1;
    } flags;
} esp_lcd_epaper_config_t;

/**
 * @brief Create an esp_lcd panel handle for an e-paper display.
 *
 * @param[in]  io               Panel IO handle, typically from esp_lcd_new_panel_io_spi()
 * @param[in]  panel_dev_config General panel config; `reset_gpio_num` is used, `vendor_config`
 *                              must point to an esp_lcd_epaper_config_t
 * @param[out] ret_panel        Resulting panel handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG or ESP_ERR_NO_MEM
 */
esp_err_t esp_lcd_new_panel_epaper(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Push the framebuffer to the glass and wait for the refresh to finish.
 *
 * Equivalent to epaper_panel_refresh_start() followed by
 * epaper_panel_refresh_wait(). Blocks for the whole panel update.
 *
 * @param[in] panel Panel handle
 * @param[in] mode  Waveform to use; EPD_REFRESH_AUTO follows the configured policy
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if BUSY never cleared
 */
esp_err_t epaper_panel_refresh(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode);

/**
 * @brief Send the framebuffer and start the waveform, without waiting for it.
 *
 * Returns once the update has begun; the update itself takes seconds. Finish
 * it with epaper_panel_refresh_wait() before drawing or refreshing again, and
 * leave the framebuffer alone until then - the controller has not yet been
 * told what is on the glass.
 *
 * A partial refresh with nothing to do also returns ESP_OK, having started
 * nothing. epaper_panel_refresh_busy() tells the two apart; waiting is
 * harmless either way.
 *
 * @param[in] panel Panel handle
 * @param[in] mode  Waveform to use; EPD_REFRESH_AUTO follows the configured policy
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if a refresh is already in flight
 */
esp_err_t epaper_panel_refresh_start(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode);

/**
 * @brief Whether a refresh started by epaper_panel_refresh_start() is still running.
 *
 * With no BUSY pin wired the driver cannot tell, and this reports true until
 * epaper_panel_refresh_wait() has run out the panel's worst-case delay.
 *
 * @param[in] panel Panel handle
 * @return true while the panel is updating
 */
bool epaper_panel_refresh_busy(esp_lcd_panel_handle_t panel);

/**
 * @brief Wait for a started refresh to finish and do the bookkeeping it needs.
 *
 * Safe to call when no refresh is pending, in which case it returns ESP_OK
 * immediately.
 *
 * @param[in] panel      Panel handle
 * @param[in] timeout_ms How long to wait, or 0 for the panel's worst case
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if BUSY never cleared
 */
esp_err_t epaper_panel_refresh_wait(esp_lcd_panel_handle_t panel, uint32_t timeout_ms);

/**
 * @brief Fill the whole framebuffer with one colour (does not refresh on its own
 *        unless `white` differs from what is on the glass and you call refresh).
 *
 * @param[in] panel Panel handle
 * @param[in] white true for a white screen, false for black
 */
esp_err_t epaper_panel_clear(esp_lcd_panel_handle_t panel, bool white);

/**
 * @brief Change orientation at runtime. Contents of the framebuffer are not re-mapped.
 */
esp_err_t epaper_panel_set_rotation(esp_lcd_panel_handle_t panel, epd_rotation_t rotation);

/** @brief Current orientation. */
epd_rotation_t epaper_panel_get_rotation(esp_lcd_panel_handle_t panel);

/** @brief Change the default waveform policy used by EPD_REFRESH_AUTO. */
esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode);

/** @brief Visible size in the current orientation. */
esp_err_t epaper_panel_get_size(esp_lcd_panel_handle_t panel, uint16_t *width, uint16_t *height);

/**
 * @brief Direct access to the internal 1bpp framebuffer, in native panel orientation.
 *
 * Row stride is (panel_width + 7) / 8 bytes: a width that is not a multiple of
 * 8, such as the 2.13" panel's 122 px, is padded and the trailing columns are
 * unused. MSB is the left-most pixel, a set bit is white (unless inverted with
 * esp_lcd_panel_invert_color()).
 */
esp_err_t epaper_panel_get_framebuffer(esp_lcd_panel_handle_t panel, uint8_t **fb, size_t *size);

/**
 * @brief Tell the driver that the panel lost power or was reset behind its back.
 *
 * Call this when the application cuts the panel rail itself, or after a deep
 * sleep wake-up where the rail was off: the controller's RAM contents are gone,
 * so the next update has to be a full one.
 */
esp_err_t epaper_panel_invalidate(esp_lcd_panel_handle_t panel);

/**
 * @brief Put the controller into deep sleep and, if a power hook is configured, cut the panel rail.
 *
 * The image stays on the glass. The next operation re-initialises the controller
 * automatically, which requires a usable RST pin.
 */
esp_err_t epaper_panel_sleep(esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif
