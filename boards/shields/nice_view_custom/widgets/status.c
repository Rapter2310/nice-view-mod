/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

extern uint8_t zmk_split_esb_display_get_layer(void);
extern uint8_t zmk_split_esb_display_get_wpm(void);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static const char *layer_names[] = {"BASE", "FN", "BT"};
#define NUM_LAYER_NAMES (sizeof(layer_names) / sizeof(layer_names[0]))

static const char *get_layer_name(uint8_t index) {
    if (index < NUM_LAYER_NAMES) {
        return layer_names[index];
    }
    return "?";
}

static void draw_top(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t label_dsc_wpm;
    init_label_dsc(&label_dsc_wpm, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);

    // fill background
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    // draw battery
    draw_battery(canvas, state);

    // draw WPM graph
    canvas_draw_rect(canvas, 0, 21, 68, 42, &rect_white_dsc);
    canvas_draw_rect(canvas, 1, 22, 66, 40, &rect_black_dsc);

    char wpm_text[6] = {};
    snprintf(wpm_text, sizeof(wpm_text), "%d", state->wpm[9]);
    canvas_draw_text(canvas, 42, 52, 24, &label_dsc_wpm, wpm_text);

    int max = 0;
    int min = 256;

    for (int i = 0; i < 10; i++) {
        if (state->wpm[i] > max) {
            max = state->wpm[i];
        }
        if (state->wpm[i] < min) {
            min = state->wpm[i];
        }
    }

    int range = max - min;
    if (range == 0) {
        range = 1;
    }

    lv_point_t points[10];
    for (int i = 0; i < 10; i++) {
        points[i].x = 2 + i * 7;
        points[i].y = 60 - (state->wpm[i] - min) * 36 / range;
    }
    canvas_draw_line(canvas, points, 10, &line_dsc);

    // rotate canvas
    rotate_canvas(canvas);
}

static void draw_bottom(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    // fill background
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    // draw layer
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        char text[10] = {};
        sprintf(text, "LAYER %i", state->layer_index);
        canvas_draw_text(canvas, 0, 5, 68, &label_dsc, text);
    } else {
        canvas_draw_text(canvas, 0, 5, 68, &label_dsc, state->layer_label);
    }

    // rotate canvas
    rotate_canvas(canvas);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    draw_top(widget->obj, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif


static void esb_display_poll_work_cb(struct k_work *work);
static K_WORK_DEFINE(esb_display_poll_work, esb_display_poll_work_cb);

static void esb_display_poll_timer_cb(struct k_timer *timer) {
    k_work_submit(&esb_display_poll_work);
}
static K_TIMER_DEFINE(esb_display_poll_timer, esb_display_poll_timer_cb, NULL);

static void esb_display_poll_work_cb(struct k_work *work) {
    uint8_t layer = zmk_split_esb_display_get_layer();
    uint8_t wpm = zmk_split_esb_display_get_wpm();

    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        // update layer
        widget->state.layer_index = layer;
        widget->state.layer_label = get_layer_name(layer);

        // shift WPM history and append new value
        for (int i = 0; i < 9; i++) {
            widget->state.wpm[i] = widget->state.wpm[i + 1];
        }
        widget->state.wpm[9] = wpm;

        // redraw
        draw_top(widget->obj, &widget->state);
        draw_bottom(widget->obj, &widget->state);
    }
}

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    memset(widget->state.wpm, 0, sizeof(widget->state.wpm));
    widget->state.layer_index = 0;
    widget->state.layer_label = NULL;

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    k_timer_start(&esb_display_poll_timer, K_SECONDS(1), K_SECONDS(1));

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
