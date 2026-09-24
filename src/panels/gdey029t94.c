/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * Good Display GDEY029T94 (also sold as GDEM029T94) - 2.9", 128x296, mono.
 * Controller: SSD1680. Timings from the Good Display datasheet, confirmed on
 * hardware: the full refresh measures 2686 ms against the 3200 ms declared
 * here, and the partial 400 ms against 500 ms, so both worst cases hold with
 * room to spare.
 */
#include "epd_private.h"

static const epd_init_cmd_t gdey029t94_init[] = {
    { 0x3C, 1, { 0x05 } },       /* border waveform: follow the LUT1 white level */
    { 0x21, 2, { 0x00, 0x80 } }, /* 128 px of glass on 176 sources: use S8..S167 */
    { 0x18, 1, { 0x80 } },       /* waveform compensation from the internal temperature sensor */
};

const epd_panel_desc_t epd_panel_gdey029t94 = {
    .name = "GDEY029T94",
    .controller = &epd_controller_ssd1680,
    .width = 128,
    .height = 296,
    .gate_lines = 296,
    .init_cmds = gdey029t94_init,
    .init_cmd_count = sizeof(gdey029t94_init) / sizeof(gdey029t94_init[0]),
    .busy_active_high = true,
    .reset_low_ms = 10,
    .reset_high_ms = 10,
    .power_on_delay_ms = 10,
    .full_refresh_ms = 3200,
    .partial_refresh_ms = 500,
    .supports_partial = true,
};
