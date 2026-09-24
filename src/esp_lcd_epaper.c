/*
 * SPDX-FileCopyrightText: 2026 Jaroslav Burian
 *
 * SPDX-License-Identifier: MIT
 *
 * esp_lcd front-end: framebuffer management, rotation and the refresh policy.
 * Everything below talks to the glass only through the controller vtable.
 */
#include <stdlib.h>
#include <string.h>

#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "epd_private.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epaper";

#define EPD_DEV(panel) __containerof(panel, epd_dev_t, base)

/* ---------------------------------------------------------------- framebuffer */

/**
 * @brief Map a coordinate in user (rotated, mirrored) space to native panel space.
 *
 * Rotation is applied on top of the panel's native portrait layout: the
 * framebuffer always stays in native orientation, only the mapping changes.
 */
static inline void epd_map_coords(const epd_dev_t *dev, int x, int y, uint16_t *nx, uint16_t *ny)
{
    const int w = dev->desc->width;
    const int h = dev->desc->height;
    int rx = x;
    int ry = y;

    switch (dev->rotation) {
    case EPD_ROTATION_90:
        rx = w - 1 - y;
        ry = x;
        break;
    case EPD_ROTATION_180:
        rx = w - 1 - x;
        ry = h - 1 - y;
        break;
    case EPD_ROTATION_270:
        rx = y;
        ry = h - 1 - x;
        break;
    case EPD_ROTATION_0:
    default:
        break;
    }
    if (dev->mirror_x) {
        rx = w - 1 - rx;
    }
    if (dev->mirror_y) {
        ry = h - 1 - ry;
    }
    *nx = (uint16_t)rx;
    *ny = (uint16_t)ry;
}

static inline void epd_fb_set(epd_dev_t *dev, uint16_t nx, uint16_t ny, bool white)
{
    uint8_t *byte = &dev->fb[(size_t)ny * dev->stride + (nx >> 3)];
    const uint8_t mask = (uint8_t)(0x80 >> (nx & 7));

    if (white) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

/** @brief Visible size in the current orientation. */
static inline void epd_visible_size(const epd_dev_t *dev, uint16_t *w, uint16_t *h)
{
    const bool landscape = (dev->rotation == EPD_ROTATION_90 || dev->rotation == EPD_ROTATION_270);
    *w = landscape ? dev->desc->height : dev->desc->width;
    *h = landscape ? dev->desc->width : dev->desc->height;
}

/* ------------------------------------------------------------- refresh policy */

/**
 * @brief Bounding box of the difference between fb and fb_prev, in native
 *        coordinates, x expanded to byte boundaries.
 *
 * @return false when the two buffers are identical
 */
static bool epd_dirty_box(const epd_dev_t *dev, uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    const uint16_t stride = dev->stride;
    int min_col = stride, max_col = -1, min_row = -1, max_row = -1;

    for (int row = 0; row < dev->desc->height; row++) {
        const uint8_t *a = dev->fb + (size_t)row * stride;
        const uint8_t *b = dev->fb_prev + (size_t)row * stride;
        if (memcmp(a, b, stride) == 0) {
            continue;
        }
        if (min_row < 0) {
            min_row = row;
        }
        max_row = row;
        for (int col = 0; col < stride; col++) {
            if (a[col] != b[col]) {
                if (col < min_col) {
                    min_col = col;
                }
                if (col > max_col) {
                    max_col = col;
                }
            }
        }
    }
    if (max_row < 0) {
        return false;
    }
    *x = (uint16_t)(min_col * 8);
    *y = (uint16_t)min_row;
    *w = (uint16_t)((max_col - min_col + 1) * 8);
    *h = (uint16_t)(max_row - min_row + 1);
    return true;
}

/** @brief Stream a native-coordinate rectangle of the framebuffer into one controller RAM. */
static esp_err_t epd_write_region(epd_dev_t *dev, epd_ram_t ram, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    const epd_controller_t *ctrl = dev->desc->controller;
    const uint16_t x_byte = x / 8;
    const uint16_t w_byte = w / 8;

    ESP_RETURN_ON_ERROR(ctrl->set_window(dev, x, y, w, h), TAG, "set window");

    if (w_byte == dev->stride) {
        /* Full-width region: one contiguous transfer. */
        return ctrl->write_ram(dev, ram, dev->fb + (size_t)y * dev->stride, (size_t)w_byte * h);
    }
    /* Partial width: the controller wraps at the window edge, so the rows can
     * still be streamed back to back, just not from contiguous memory. */
    for (uint16_t row = 0; row < h; row++) {
        const uint8_t *src = dev->fb + (size_t)(y + row) * dev->stride + x_byte;
        ESP_RETURN_ON_ERROR(ctrl->write_ram(dev, ram, src, w_byte), TAG, "write ram");
    }
    return ESP_OK;
}

/**
 * @brief Ask the board to switch the panel rail, and track what that does to
 *        the controller's state.
 */
static esp_err_t epd_power(epd_dev_t *dev, bool on)
{
    if (!dev->power_ctrl) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(dev->power_ctrl(on, dev->power_ctrl_ctx), TAG, "power control");
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(dev->desc->power_on_delay_ms));
    } else {
        /* The controller loses its RAM with the rail, so the next update has to
         * repaint everything from scratch. */
        dev->init_done = false;
        dev->hibernating = false;
        dev->power_on = false;
        dev->prev_valid = false;
        dev->refresh_pending = false;
    }
    return ESP_OK;
}

static esp_err_t epd_ensure_init(epd_dev_t *dev)
{
    if (dev->init_done && !dev->hibernating) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(dev->desc->controller->init(dev), TAG, "controller init");
    dev->init_done = true;
    return ESP_OK;
}

static esp_err_t epd_refresh_full_start(epd_dev_t *dev)
{
    const epd_controller_t *ctrl = dev->desc->controller;
    const uint16_t w = dev->stride * 8; /* whole RAM rows, padding included */
    const uint16_t h = dev->desc->height;

    /* Both RAMs get the new image: the flashing waveform does not need a
     * previous image, and syncing them keeps later partial updates correct. */
    ESP_RETURN_ON_ERROR(epd_write_region(dev, EPD_RAM_PREVIOUS, 0, 0, w, h), TAG, "prev ram");
    ESP_RETURN_ON_ERROR(epd_write_region(dev, EPD_RAM_CURRENT, 0, 0, w, h), TAG, "cur ram");
    ESP_RETURN_ON_ERROR(ctrl->refresh_start(dev, false), TAG, "refresh");

    dev->refresh_pending = true;
    dev->refresh_partial = false;
    return ESP_OK;
}

static esp_err_t epd_refresh_partial_start(epd_dev_t *dev)
{
    const epd_controller_t *ctrl = dev->desc->controller;
    uint16_t x, y, w, h;

    if (!epd_dirty_box(dev, &x, &y, &w, &h)) {
        ESP_LOGD(TAG, "nothing changed, skipping refresh");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(epd_write_region(dev, EPD_RAM_CURRENT, x, y, w, h), TAG, "cur ram");
    ESP_RETURN_ON_ERROR(ctrl->refresh_start(dev, true), TAG, "refresh");

    /* Remembered so that the wait half can hand the controller the same region
     * as the image now on the glass. */
    dev->dirty_x = x;
    dev->dirty_y = y;
    dev->dirty_w = w;
    dev->dirty_h = h;
    dev->refresh_pending = true;
    dev->refresh_partial = true;
    return ESP_OK;
}

/** @brief Bookkeeping that can only be done once the waveform has finished. */
static esp_err_t epd_refresh_finish(epd_dev_t *dev)
{
    if (dev->refresh_partial) {
        /* Hand the controller the image that is now on the glass, so the next
         * differential update starts from the right baseline. */
        ESP_RETURN_ON_ERROR(epd_write_region(dev,
                                             EPD_RAM_PREVIOUS,
                                             dev->dirty_x,
                                             dev->dirty_y,
                                             dev->dirty_w,
                                             dev->dirty_h),
                            TAG,
                            "prev ram");
        dev->partial_count++;
    } else {
        dev->prev_valid = true;
        dev->partial_count = 0;
    }
    memcpy(dev->fb_prev, dev->fb, dev->fb_size);
    return ESP_OK;
}

/* ------------------------------------------------------- esp_lcd panel vtable */

static esp_err_t panel_epaper_reset(esp_lcd_panel_t *panel)
{
    epd_dev_t *dev = EPD_DEV(panel);

    epd_hw_reset(dev);
    dev->init_done = false;
    dev->hibernating = false;
    dev->power_on = false;
    dev->prev_valid = false;
    dev->refresh_pending = false;
    return ESP_OK;
}

static esp_err_t panel_epaper_init(esp_lcd_panel_t *panel)
{
    epd_dev_t *dev = EPD_DEV(panel);
    return epd_ensure_init(dev);
}

static esp_err_t panel_epaper_del(esp_lcd_panel_t *panel)
{
    epd_dev_t *dev = EPD_DEV(panel);

    epaper_panel_refresh_wait(panel, 0);
    if (dev->init_done && !dev->hibernating) {
        dev->desc->controller->sleep(dev);
    }
    epd_power(dev, false);
    free(dev->fb);
    free(dev->fb_prev);
    free(dev->bounce);
    free(dev);
    return ESP_OK;
}

static esp_err_t
panel_epaper_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    epd_dev_t *dev = EPD_DEV(panel);
    const uint8_t *src = (const uint8_t *)color_data;
    uint16_t vis_w, vis_h;

    ESP_RETURN_ON_FALSE(color_data, ESP_ERR_INVALID_ARG, TAG, "null bitmap");
    ESP_RETURN_ON_FALSE(x_end > x_start && y_end > y_start, ESP_ERR_INVALID_ARG, TAG, "empty area");
    /* Anything drawn now would be folded into the record of what is on the
     * glass when the pending refresh is waited on, and never actually shown. */
    ESP_RETURN_ON_FALSE(!dev->refresh_pending, ESP_ERR_INVALID_STATE, TAG, "a refresh is running, wait for it first");

    x_start += dev->gap_x;
    x_end += dev->gap_x;
    y_start += dev->gap_y;
    y_end += dev->gap_y;

    /* Source rows are padded to whole bytes, MSB is the left-most pixel - the
     * layout produced by LVGL's I1 colour format and by esp_lcd in general for
     * 1bpp panels. The stride describes the rectangle the caller passed, so it
     * is measured before clipping; clipping then only changes which part of
     * that bitmap is read, never how it is laid out. */
    const int src_stride = (x_end - x_start + 7) / 8;
    const int src_x0 = x_start;
    const int src_y0 = y_start;

    epd_visible_size(dev, &vis_w, &vis_h);
    if (x_start < 0) {
        x_start = 0;
    }
    if (y_start < 0) {
        y_start = 0;
    }
    if (x_end > vis_w) {
        x_end = vis_w;
    }
    if (y_end > vis_h) {
        y_end = vis_h;
    }
    if (x_end <= x_start || y_end <= y_start) {
        return ESP_OK;
    }

    for (int y = y_start; y < y_end; y++) {
        const uint8_t *src_row = src + (size_t)(y - src_y0) * src_stride;
        for (int x = x_start; x < x_end; x++) {
            const int col = x - src_x0;
            bool bit = (src_row[col >> 3] >> (7 - (col & 7))) & 1;
            uint16_t nx, ny;
            epd_map_coords(dev, x, y, &nx, &ny);
            epd_fb_set(dev, nx, ny, dev->invert ? !bit : bit);
        }
    }

    if (dev->auto_refresh) {
        return epaper_panel_refresh(panel, EPD_REFRESH_AUTO);
    }
    return ESP_OK;
}

static esp_err_t panel_epaper_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    epd_dev_t *dev = EPD_DEV(panel);

    dev->mirror_x = mirror_x;
    dev->mirror_y = mirror_y;
    return ESP_OK;
}

static esp_err_t panel_epaper_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    epd_dev_t *dev = EPD_DEV(panel);

    /* Expressed through the rotation state so that both APIs stay consistent. */
    const bool landscape = (dev->rotation == EPD_ROTATION_90 || dev->rotation == EPD_ROTATION_270);
    if (swap_axes == landscape) {
        return ESP_OK;
    }
    /* Swap the axes while keeping which way up the panel is, so that a panel
     * mounted at 180 or 270 degrees does not silently flip when a UI framework
     * toggles the axes: 0 <-> 90 and 180 <-> 270. */
    switch (dev->rotation) {
    case EPD_ROTATION_0:
        dev->rotation = EPD_ROTATION_90;
        break;
    case EPD_ROTATION_90:
        dev->rotation = EPD_ROTATION_0;
        break;
    case EPD_ROTATION_180:
        dev->rotation = EPD_ROTATION_270;
        break;
    case EPD_ROTATION_270:
        dev->rotation = EPD_ROTATION_180;
        break;
    }
    return ESP_OK;
}

static esp_err_t panel_epaper_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    epd_dev_t *dev = EPD_DEV(panel);

    dev->gap_x = x_gap;
    dev->gap_y = y_gap;
    return ESP_OK;
}

static esp_err_t panel_epaper_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    epd_dev_t *dev = EPD_DEV(panel);

    dev->invert = invert;
    return ESP_OK;
}

static esp_err_t panel_epaper_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    epd_dev_t *dev = EPD_DEV(panel);

    if (on) {
        ESP_RETURN_ON_ERROR(epd_power(dev, true), TAG, "power on");
        return epd_ensure_init(dev);
    }
    return epaper_panel_sleep(panel);
}

static esp_err_t panel_epaper_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    return sleep ? epaper_panel_sleep(panel) : panel_epaper_disp_on_off(panel, true);
}

/* ------------------------------------------------------------------ public API */

esp_err_t esp_lcd_new_panel_epaper(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    epd_dev_t *dev = NULL;

    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "null argument");
    const esp_lcd_epaper_config_t *cfg = (const esp_lcd_epaper_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(cfg && cfg->panel, ESP_ERR_INVALID_ARG, TAG, "vendor_config must name a panel");
    const epd_panel_desc_t *desc = cfg->panel;
    /* 0 means the caller left it unset; anything other than 1 bpp would be
     * silently misinterpreted by draw_bitmap(). */
    ESP_RETURN_ON_FALSE(panel_dev_config->bits_per_pixel <= 1,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "e-paper panels are 1 bpp, got %d",
                        (int)panel_dev_config->bits_per_pixel);

    dev = calloc(1, sizeof(epd_dev_t));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "no mem for panel");

    dev->io = io;
    dev->desc = desc;
    dev->reset_gpio = panel_dev_config->reset_gpio_num;
    dev->busy_gpio = cfg->busy_gpio_num;
    dev->power_ctrl = cfg->power_ctrl;
    dev->power_ctrl_ctx = cfg->power_ctrl_ctx;
    dev->rotation = cfg->rotation;
    dev->mode = cfg->refresh_mode;
    dev->full_refresh_interval = cfg->full_refresh_interval;
    dev->auto_refresh = cfg->auto_refresh;
    /* Controller RAM is organised in bytes, so a panel like the 122 px wide
     * 2.13" one occupies 16 bytes per row and the last 6 columns are unused. */
    dev->stride = (desc->width + 7) / 8;
    dev->fb_size = (size_t)dev->stride * desc->height;

    if (!desc->supports_partial && dev->mode != EPD_REFRESH_FULL) {
        ESP_LOGW(TAG, "%s has no partial mode, falling back to full refresh", desc->name);
        dev->mode = EPD_REFRESH_FULL;
    }

    /* Only `fb` reaches the SPI driver, so only `fb` wants DMA-capable memory.
     * Two screen-sized buffers stop fitting internal RAM well before the
     * largest panels, so fall back to any heap and stage the transfers. */
    uint32_t fb_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    uint32_t prev_caps = MALLOC_CAP_8BIT;

    if (cfg->flags.fb_in_psram) {
        fb_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        prev_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    dev->fb = heap_caps_malloc(dev->fb_size, fb_caps);
    if (!dev->fb) {
        dev->fb = heap_caps_malloc(dev->fb_size, MALLOC_CAP_8BIT);
    }
    /* `fb_prev` is compared and copied by the CPU only, never sent. */
    dev->fb_prev = heap_caps_malloc(dev->fb_size, prev_caps);
    if (!dev->fb_prev) {
        dev->fb_prev = heap_caps_malloc(dev->fb_size, MALLOC_CAP_8BIT);
    }
    ESP_GOTO_ON_FALSE(dev->fb && dev->fb_prev, ESP_ERR_NO_MEM, err, TAG, "no mem for framebuffer");

    if (!esp_ptr_dma_capable(dev->fb)) {
        dev->bounce = heap_caps_malloc(EPD_TX_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        ESP_GOTO_ON_FALSE(dev->bounce, ESP_ERR_NO_MEM, err, TAG, "no mem for bounce buffer");
        ESP_LOGD(TAG, "framebuffer is not DMA-capable, staging through %d B", EPD_TX_CHUNK);
    }
    memset(dev->fb, 0xFF, dev->fb_size); /* white */
    memset(dev->fb_prev, 0xFF, dev->fb_size);

    if (dev->reset_gpio >= 0) {
        gpio_config_t out_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << dev->reset_gpio,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&out_cfg), err, TAG, "configure reset pin");
    }
    if (dev->busy_gpio >= 0) {
        gpio_config_t busy_cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << dev->busy_gpio,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&busy_cfg), err, TAG, "configure busy pin");
    }
    ESP_GOTO_ON_ERROR(epd_power(dev, true), err, TAG, "power on");

    dev->base.reset = panel_epaper_reset;
    dev->base.init = panel_epaper_init;
    dev->base.del = panel_epaper_del;
    dev->base.draw_bitmap = panel_epaper_draw_bitmap;
    dev->base.mirror = panel_epaper_mirror;
    dev->base.swap_xy = panel_epaper_swap_xy;
    dev->base.set_gap = panel_epaper_set_gap;
    dev->base.invert_color = panel_epaper_invert_color;
    dev->base.disp_on_off = panel_epaper_disp_on_off;
    dev->base.disp_sleep = panel_epaper_disp_sleep;

    *ret_panel = &dev->base;
    ESP_LOGI(TAG, "%s (%s) %dx%d ready", desc->name, desc->controller->name, desc->width, desc->height);
    return ESP_OK;

err:
    if (dev) {
        free(dev->fb);
        free(dev->fb_prev);
        free(dev->bounce);
        free(dev);
    }
    return ret;
}

esp_err_t epaper_panel_refresh_start(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    epd_dev_t *dev = EPD_DEV(panel);

    ESP_RETURN_ON_FALSE(!dev->refresh_pending, ESP_ERR_INVALID_STATE, TAG, "a refresh is already running");
    ESP_RETURN_ON_ERROR(epd_ensure_init(dev), TAG, "init");

    if (mode == EPD_REFRESH_AUTO) {
        mode = dev->mode == EPD_REFRESH_AUTO ? EPD_REFRESH_PARTIAL : dev->mode;
    }
    /* The first update after power-up or deep sleep must be a full one: the
     * controller's previous-image RAM does not match the glass. */
    const bool interval_due = dev->full_refresh_interval && dev->partial_count >= dev->full_refresh_interval;
    if (!dev->prev_valid || !dev->desc->supports_partial || interval_due) {
        mode = EPD_REFRESH_FULL;
    }

    esp_err_t err;

    if (mode == EPD_REFRESH_FULL) {
        err = epd_refresh_full_start(dev);
    } else {
        err = epd_refresh_partial_start(dev);
    }
    dev->refresh_start_us = esp_timer_get_time();
    return err;
}

bool epaper_panel_refresh_busy(esp_lcd_panel_handle_t panel)
{
    if (!panel) {
        return false;
    }
    epd_dev_t *dev = EPD_DEV(panel);

    if (!dev->refresh_pending) {
        return false;
    }
    if (dev->busy_gpio >= 0) {
        return epd_busy_asserted(dev);
    }
    /* No BUSY pin to read, so go by the clock instead. Reporting "busy" until
     * the panel's worst case has elapsed keeps a poll loop terminating, which
     * an unconditional true would not. */
    const uint32_t worst_ms = dev->refresh_partial ? dev->desc->partial_refresh_ms : dev->desc->full_refresh_ms;
    return (esp_timer_get_time() - dev->refresh_start_us) < (int64_t)worst_ms * 1000;
}

esp_err_t epaper_panel_refresh_wait(esp_lcd_panel_handle_t panel, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    epd_dev_t *dev = EPD_DEV(panel);

    if (!dev->refresh_pending) {
        return ESP_OK;
    }
    const uint32_t busy_ms = dev->refresh_partial ? dev->desc->partial_refresh_ms : dev->desc->full_refresh_ms;
    if (timeout_ms == 0) {
        timeout_ms = busy_ms + 1000;
    }
    ESP_RETURN_ON_ERROR(epd_wait_busy(dev, busy_ms, timeout_ms), TAG, "busy during refresh");

    /* Cleared before the bookkeeping so that a failure there does not leave the
     * panel permanently marked busy. */
    dev->refresh_pending = false;
    return epd_refresh_finish(dev);
}

esp_err_t epaper_panel_refresh(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode)
{
    ESP_RETURN_ON_ERROR(epaper_panel_refresh_start(panel, mode), TAG, "refresh start");
    return epaper_panel_refresh_wait(panel, 0);
}

esp_err_t epaper_panel_clear(esp_lcd_panel_handle_t panel, bool white)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    epd_dev_t *dev = EPD_DEV(panel);

    ESP_RETURN_ON_FALSE(!dev->refresh_pending, ESP_ERR_INVALID_STATE, TAG, "a refresh is running, wait for it first");
    memset(dev->fb, white ? 0xFF : 0x00, dev->fb_size);
    return ESP_OK;
}

esp_err_t epaper_panel_set_rotation(esp_lcd_panel_handle_t panel, epd_rotation_t rotation)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    ESP_RETURN_ON_FALSE(rotation <= EPD_ROTATION_270, ESP_ERR_INVALID_ARG, TAG, "bad rotation");
    EPD_DEV(panel)->rotation = rotation;
    return ESP_OK;
}

epd_rotation_t epaper_panel_get_rotation(esp_lcd_panel_handle_t panel)
{
    return EPD_DEV(panel)->rotation;
}

esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_handle_t panel, epd_refresh_mode_t mode)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    EPD_DEV(panel)->mode = mode;
    return ESP_OK;
}

esp_err_t epaper_panel_get_size(esp_lcd_panel_handle_t panel, uint16_t *width, uint16_t *height)
{
    ESP_RETURN_ON_FALSE(panel && width && height, ESP_ERR_INVALID_ARG, TAG, "null argument");
    epd_visible_size(EPD_DEV(panel), width, height);
    return ESP_OK;
}

esp_err_t epaper_panel_get_framebuffer(esp_lcd_panel_handle_t panel, uint8_t **fb, size_t *size)
{
    ESP_RETURN_ON_FALSE(panel && fb, ESP_ERR_INVALID_ARG, TAG, "null argument");
    epd_dev_t *dev = EPD_DEV(panel);

    *fb = dev->fb;
    if (size) {
        *size = dev->fb_size;
    }
    return ESP_OK;
}

esp_err_t epaper_panel_sleep(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    epd_dev_t *dev = EPD_DEV(panel);

    /* Cutting the rail mid-waveform would leave a half-drawn image on the
     * glass, so finish what is running first. */
    ESP_RETURN_ON_ERROR(epaper_panel_refresh_wait(panel, 0), TAG, "pending refresh");
    if (dev->init_done) {
        ESP_RETURN_ON_ERROR(dev->desc->controller->sleep(dev), TAG, "sleep");
    }
    return epd_power(dev, false);
}

esp_err_t epaper_panel_invalidate(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "null panel");
    epd_dev_t *dev = EPD_DEV(panel);

    dev->init_done = false;
    dev->hibernating = false;
    dev->power_on = false;
    dev->prev_valid = false;
    dev->refresh_pending = false;
    return ESP_OK;
}
