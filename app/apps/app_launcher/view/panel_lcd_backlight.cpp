/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"
#include <algorithm>
#include <lvgl.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <apps/utils/audio/audio.h>

using namespace launcher_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string _tag = "panel-lcd-bl";

static constexpr int16_t _label_pos_x      = 595;
static constexpr int16_t _label_pos_y      = 107;
static constexpr int16_t _label_panel_ic_x = -7;
static constexpr int16_t _label_panel_ic_y = 74;
static constexpr int16_t _btn_up_pos_x     = 505;
static constexpr int16_t _btn_up_pos_y     = 176;
static constexpr int16_t _btn_down_pos_x   = 599;
static constexpr int16_t _btn_down_pos_y   = 176;

static constexpr int16_t _midi_up     = 64 + 24;
static constexpr int16_t _midi_down   = 60 + 24;
static constexpr int16_t _midi_top    = 64 + 8 + 24;
static constexpr int16_t _midi_bottom = 60 - 8 + 24;

void PanelLcdBacklight::init()
{
    _label_brightness = std::make_unique<Label>(lv_screen_active());
    _label_brightness->align(LV_ALIGN_CENTER, _label_pos_x, _label_pos_y);
    _label_brightness->setTextFont(&lv_font_montserrat_36);
    _label_brightness->setTextColor(lv_color_hex(0xFEFEFE));
    _label_brightness->setText(fmt::format("{}", GetHAL()->getDisplayBrightness()));

    _label_panel_ic = std::make_unique<Label>(lv_screen_active());
    _label_panel_ic->align(LV_ALIGN_RIGHT_MID, _label_panel_ic_x, _label_panel_ic_y);
    _label_panel_ic->setTextFont(&lv_font_montserrat_16);
    _label_panel_ic->setTextColor(lv_color_hex(0x55866D));
    _label_panel_ic->setText(fmt::format("LCD IC: {}", GetHAL()->getDisplayPanelIc()));

    _btn_up = std::make_unique<Container>(lv_screen_active());
    _btn_up->align(LV_ALIGN_CENTER, _btn_up_pos_x, _btn_up_pos_y);
    _btn_up->setSize(81, 85);
    _btn_up->setOpa(0);
    _btn_up->onClick().connect([&]() {
        // Animation
        _label_y_anim.teleport(_label_pos_y - 8);
        _label_y_anim = _label_pos_y;

        // SFX
        if (GetHAL()->getDisplayBrightness() >= 100) {
            audio::play_tone_from_midi(_midi_top);
            return;
        }
        audio::play_tone_from_midi(_midi_up);

        // Update brightness
        int target = GetHAL()->getDisplayBrightness();
        target     = std::clamp(target + 20, 0, 100);
        GetHAL()->setDisplayBrightness(target);
        _label_brightness->setText(fmt::format("{}", GetHAL()->getDisplayBrightness()));
    });

    _btn_down = std::make_unique<Container>(lv_screen_active());
    _btn_down->align(LV_ALIGN_CENTER, _btn_down_pos_x, _btn_down_pos_y);
    _btn_down->setSize(81, 85);
    _btn_down->setOpa(0);
    _btn_down->onClick().connect([&]() {
        // Animation
        _label_y_anim.teleport(_label_pos_y + 8);
        _label_y_anim = _label_pos_y;

        // SFX
        if (GetHAL()->getDisplayBrightness() <= 20) {
            audio::play_tone_from_midi(_midi_bottom);
            return;
        }
        audio::play_tone_from_midi(_midi_down);

        // Update brightness
        int target = GetHAL()->getDisplayBrightness();
        target     = std::clamp(target - 20, 20, 100);
        GetHAL()->setDisplayBrightness(target);
        _label_brightness->setText(fmt::format("{}", GetHAL()->getDisplayBrightness()));
    });

    _label_y_anim.easingOptions().duration = 0.2;
    _label_y_anim.teleport(_label_pos_y);
}

void PanelLcdBacklight::update(bool isStacked)
{
    if (!_label_y_anim.done()) {
        _label_brightness->setY(_label_y_anim);
    }
}
