/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Panel catalogue of the esp_epaper component, and the descriptor type needed
 * to add a panel of your own.
 *
 * A "panel" is a concrete display module: glass, controller and timings.
 * Panels that ship with the component are declared at the bottom of this file.
 *
 * A panel whose controller is already supported does not need a fork: fill in
 * an epd_panel_desc_t in your own application, point it at one of the
 * epd_controller_* symbols below, and pass it as
 * esp_lcd_epaper_config_t.panel. A new controller family does need a driver
 * inside the component - the controller interface is deliberately internal, so
 * that it can keep evolving.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A controller driver, referenced by a panel descriptor.
 *
 * Opaque on purpose: a descriptor only needs the address of one, and keeping
 * the contents internal leaves the component free to change how controllers
 * are written.
 */
typedef struct epd_controller_t epd_controller_t;

/** @brief Solomon Systech SSD1680 / SSD1681 family. */
extern const epd_controller_t epd_controller_ssd1680;

/** @brief One command of a panel's initialisation sequence. */
typedef struct {
    uint8_t cmd;     /*!< Controller command byte */
    uint8_t len;     /*!< Number of parameter bytes, 0 to 4 */
    uint8_t data[4]; /*!< Parameter bytes */
} epd_init_cmd_t;

/**
 * @brief Static description of a display module.
 *
 * The geometry and timing fields carry meaning the driver acts on; the
 * initialisation table is passed through to the controller untouched, which is
 * where panel quirks belong. Leaving a field zero (or the table empty) selects
 * the controller's own default.
 */
typedef struct {
    const char *name;                   /*!< Shown in the log line at init */
    const epd_controller_t *controller; /*!< e.g. &epd_controller_ssd1680 */
    uint16_t width;                     /*!< Visible native width in pixels, any value */
    uint16_t height;                    /*!< Native height in pixels */
    uint16_t gate_lines;                /*!< Value programmed into driver output control (usually height) */
    /**
     * Commands sent after the controller's common reset and geometry setup and
     * before the first RAM window is selected - border waveform, source range,
     * temperature sensor, gate and source voltages. NULL for none.
     */
    const epd_init_cmd_t *init_cmds;
    size_t init_cmd_count;       /*!< Number of entries in `init_cmds` */
    bool busy_active_high;       /*!< true when BUSY is high while the panel is working */
    uint16_t reset_low_ms;       /*!< Hardware reset pulse width */
    uint16_t reset_high_ms;      /*!< Settling time after the reset pulse */
    uint16_t power_on_delay_ms;  /*!< Settling time after the power rail is switched on */
    uint32_t full_refresh_ms;    /*!< Worst-case full refresh duration, used as BUSY timeout */
    uint32_t partial_refresh_ms; /*!< Worst-case partial refresh duration */
    bool supports_partial;       /*!< Whether the panel has a fast differential update mode */
} epd_panel_desc_t;

/* ------------------------------------------------------------ shipped panels */

/**
 * @brief Good Display GDEY029T94 / GDEM029T94 - 2.9" 128x296 mono, SSD1680.
 *
 * Used e.g. on the LaskaKit ESPink-Shelf-2.9. Supports full refresh and
 * fast differential partial refresh (controller display mode 2).
 */
extern const epd_panel_desc_t epd_panel_gdey029t94;

/**
 * @brief Good Display GDEY0213B74 / GDEM0213B74 - 2.13" 122x250 mono, SSD1680.
 *
 * @warning Written from the datasheet and not verified on hardware. If you have
 *          this panel, a report either way is welcome.
 */
extern const epd_panel_desc_t epd_panel_gdey0213b74;

/**
 * @brief Good Display GDEH0154D67 / GDEY0154D67 - 1.54" 200x200 mono, SSD1681.
 *
 * @warning Written from the datasheet and not verified on hardware. If you have
 *          this panel, a report either way is welcome.
 */
extern const epd_panel_desc_t epd_panel_gdeh0154d67;

#ifdef __cplusplus
}
#endif
