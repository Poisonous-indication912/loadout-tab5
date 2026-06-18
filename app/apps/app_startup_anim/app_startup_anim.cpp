/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#include "app_startup_anim.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <string>

using namespace mooncake;

#define COL_BG    0x070b0a
#define COL_SCRAM 0x2f8f5f   // verde apagado (caracteres aun cifrados)
#define COL_LOCK  0x38ef7d   // verde fósforo (resuelto)
#define COL_OUT   0xffae00   // ámbar (marca, al final)
#define COL_DIM   0x6f8f7f   // estado

static const char* TARGET   = "// LOADOUT";
static const int   TLEN     = 10;    // longitud de "// LOADOUT" (incluye '/','/',' ')
static const char* GLYPHS   = "!<>-_\\/[]{}=+*#%&$01ABCDEF@?XZ";
static const int   GLYPH_N  = 30;

static const int LOCK_MS   = 170;    // ms por carácter desencriptado (~1.7s total)
static const int SCRAM_MS  = 45;     // refresco del scramble
static const int HOLD_MS   = 450;

AppStartupAnim::AppStartupAnim()
{
    setAppInfo().name = "AppStartupAnim";
}

void AppStartupAnim::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppStartupAnim::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    LvglLockGuard lock;

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    _load = lv_label_create(scr);
    lv_label_set_text(_load, "");
    lv_obj_set_style_text_font(_load, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_load, lv_color_hex(COL_SCRAM), 0);
    lv_obj_align(_load, LV_ALIGN_CENTER, 0, -16);

    _status = lv_label_create(scr);
    lv_label_set_text(_status, "");
    lv_obj_set_style_text_font(_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_status, lv_color_hex(COL_DIM), 0);
    lv_obj_align(_status, LV_ALIGN_CENTER, 0, 48);

    _t0 = _tstep = GetHAL()->millis();
}

void AppStartupAnim::onRunning()
{
    uint32_t now = GetHAL()->millis();
    LvglLockGuard lock;

    // SFX de "decoding" al arrancar la animación.
    if (!_sfx_started) { GetHAL()->playDecodeSfx(); _sfx_started = true; }

    switch (_state) {
    case S_DECRYPT: {
        uint32_t e   = now - _t0;
        int locked   = (int)(e / LOCK_MS);
        if (locked > TLEN) locked = TLEN;

        if (now - _tstep >= SCRAM_MS) {
            _tstep = now;
            std::string s;
            for (int i = 0; i < TLEN; i++) {
                char tc = TARGET[i];
                if (tc == ' ' || tc == '/') { s += tc; continue; }  // separadores fijos
                if (i < locked) {
                    s += tc;
                } else {
                    _rng = _rng * 1664525u + 1013904223u;
                    s += GLYPHS[_rng % GLYPH_N];
                }
            }
            lv_label_set_text(_load, s.c_str());

            // barra de progreso
            std::string bar = "LOADING [";
            for (int i = 0; i < TLEN; i++) bar += (i < locked) ? '#' : '.';
            bar += "]";
            lv_label_set_text(_status, bar.c_str());
        }

        if (locked >= TLEN && e >= (uint32_t)(TLEN * LOCK_MS)) {
            lv_label_set_text(_load, TARGET);
            lv_obj_set_style_text_color(_load, lv_color_hex(COL_OUT), 0);  // marca en ámbar
            lv_label_set_text(_status, "READY");
            if (!_wifi_started) { GetHAL()->startWifiAp(); _wifi_started = true; }
            _state = S_HOLD; _t0 = now;
        }
        break;
    }
    case S_HOLD: {
        if (now - _t0 >= (uint32_t)HOLD_MS) { close(); }
        break;
    }
    }
}

void AppStartupAnim::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    LvglLockGuard lock;
    if (_load)   { lv_obj_del(_load);   _load = nullptr; }
    if (_status) { lv_obj_del(_status); _status = nullptr; }
}
