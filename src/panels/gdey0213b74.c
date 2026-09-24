/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Good Display GDEY0213B74 (also sold as GDEM0213B74) - 2.13", 122x250, mono.
 * Controller: SSD1680.
 *
 * The glass is 122 px wide on a 128 px RAM row, so the framebuffer stride is
 * 16 bytes and the last 6 columns of each row are never displayed.
 *
 * NOT VERIFIED ON HARDWARE - see the note in README.md.
 */
#include "epd_private.h"

static const epd_init_cmd_t gdey0213b74_init[] = {
    { 0x3C, 1, { 0x05 } },       /* border waveform: follow the LUT1 white level */
    { 0x21, 2, { 0x00, 0x80 } }, /* 128 px of glass on 176 sources: use S8..S167 */
    { 0x18, 1, { 0x80 } },       /* waveform compensation from the internal temperature sensor */
};

const epd_panel_desc_t epd_panel_gdey0213b74 = {
    .name = "GDEY0213B74",
    .controller = &epd_controller_ssd1680,
    .width = 122,
    .height = 250,
    .gate_lines = 250,
    .init_cmds = gdey0213b74_init,
    .init_cmd_count = sizeof(gdey0213b74_init) / sizeof(gdey0213b74_init[0]),
    .busy_active_high = true,
    .reset_low_ms = 10,
    .reset_high_ms = 10,
    .power_on_delay_ms = 10,
    .full_refresh_ms = 3600,
    .partial_refresh_ms = 500,
    .supports_partial = true,
};
