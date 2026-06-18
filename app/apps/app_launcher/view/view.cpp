/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 *
 * LOADOUT launcher — widgets reales.
 *  - Barra superior derecha: hora (RTC), bateria %, power V/A (status, en vivo).
 *  - Botones rapidos verticales a la derecha: brillo y volumen.
 *  - Rejilla 3x3 de tarjetas: LAUNCHER, OTA, FILE BROWSER, CAMERA, IMU, GPIO,
 *    I2C, MUSIC, UART. LAUNCHER -> firmwares; IMU -> osciloscopio; resto -> modal.
 */
#include "view.h"
#include <lvgl.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <apps/utils/audio/audio.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <mutex>
#include <vector>
#include <functional>
#if __has_include("loadout_version.h")
#include "loadout_version.h"
#endif
#ifndef LOADOUT_VERSION
#define LOADOUT_VERSION "dev"
#endif
#include "assets/fa_icons.h"   // iconos FontAwesome (fuente fa_icons + macros FA_*)

using namespace launcher_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string _tag = "launcher-view";

// Tema (theme changer). Los colores son variables de runtime: se cargan en
// LauncherView::init desde NVS ("theme") y persisten. Cambiar tema = reiniciar
// para repintar todo. Default = el verde fósforo original.
struct Theme {
    const char* name;
    uint32_t bg, card, border, title, label, icon, value, accent;
};
static const Theme THEMES[] = {
    // name             bg        card      border    title     label     icon      value     accent
    {"Default",        0x070b0a, 0x0f1a16, 0x2f6f4f, 0xbfe9cf, 0xcfeede, 0x9be8b8, 0xe8fff0, 0x38ef7d},
    {"Solarized Dark", 0x002b36, 0x073642, 0x586e75, 0x93a1a1, 0xeee8d5, 0x839496, 0xfdf6e3, 0x2aa198},
    {"Solarized Light",0xfdf6e3, 0xeee8d5, 0x93a1a1, 0x586e75, 0x073642, 0x657b83, 0x002b36, 0xb58900},
    {"Amber Mono",     0x0a0700, 0x1a1305, 0x6f5a2f, 0xffd479, 0xf0e6c8, 0xc8a85a, 0xfff4d6, 0xffae00},
};
static const int THEME_COUNT = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
static int      g_theme  = 0;

static uint32_t COL_BG     = 0x070b0a;
static uint32_t COL_CARD   = 0x0f1a16;
static uint32_t COL_BORDER = 0x2f6f4f;
static uint32_t COL_ACCENT = 0x38ef7d;   // acento del tema
static uint32_t COL_ICON   = 0x9be8b8;
static uint32_t COL_LABEL  = 0xcfeede;
static uint32_t COL_TITLE  = 0xbfe9cf;
static uint32_t COL_VALUE  = 0xe8fff0;

static void apply_theme(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) idx = 0;
    g_theme = idx;
    const Theme& t = THEMES[idx];
    COL_BG = t.bg; COL_CARD = t.card; COL_BORDER = t.border; COL_TITLE = t.title;
    COL_LABEL = t.label; COL_ICON = t.icon; COL_VALUE = t.value; COL_ACCENT = t.accent;
}

/* tarjetas del grid (orden de la rejilla) */
// Tiles visibles del grid principal (los primeros G_COUNT). Las herramientas
// (IMU/GPIO/I2C/UART/Power) ya NO van en el grid: son IDs de routing que abre
// la tarjeta "Tools" del sidebar.
enum { G_LAUNCHER, G_OTA, G_FILES, G_CAMERA, G_MUSIC, G_ASK, G_COUNT,
       G_IMU, G_GPIO, G_I2C, G_UART, G_POWERINFO };
struct TileDef { const char* icon; const char* name; };
static const TileDef GRID[G_COUNT] = {
    {FA_LAUNCHER, "LAUNCHER"},
    {FA_OTA,      "OTA UPDATE"},
    {FA_FILES,    "FILE BROWSER"},
    {FA_CAMERA,   "CAMERA"},
    {FA_MUSIC,    "MUSIC"},
    {FA_ASK,      "ASK AI"},
};

/* acciones (indices logicos para open_tile) */
enum { A_BRIGHT = 100, A_SPEAKER = 101, A_POWER = 102, A_WIFI = 103,
       A_SETTINGS = 104, A_ABOUT = 105, A_TOOLS = 106 };
static const int TOOL_SCREENREC = -200;  // accion especial (no abre ventana)

// status bar (en vivo)
static lv_obj_t* s_time = nullptr;
static lv_obj_t* s_batt = nullptr;
static lv_obj_t* s_pow  = nullptr;
static lv_obj_t* s_kb   = nullptr;   // indicador de teclado conectado
static lv_obj_t* s_bigclock = nullptr;  // reloj grande (equilibra el home)
static lv_obj_t* s_bigdate  = nullptr;

static bool is_bin(const std::string& n)
{
    if (n.size() < 4) return false;
    std::string e = n.substr(n.size() - 4);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e == ".bin";
}

static bool is_text(const std::string& n)
{
    static const char* exts[] = {".txt", ".cfg", ".ini", ".json", ".log", ".md", ".csv", ".conf"};
    for (auto ext : exts) {
        size_t le = strlen(ext);
        if (n.size() > le) {
            std::string s = n.substr(n.size() - le);
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            if (s == ext) return true;
        }
    }
    return false;
}

static LauncherView* s_view = nullptr;    // instancia activa (para callbacks)

/* ----------------------------- modal genérico ----------------------------- */
class InfoWindow : public ui::Window {
public:
    InfoWindow(const std::string& title, const std::string& body)
    {
        config.title = title; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 820, 460, 255};
        _body = body;
    }
    void onOpen() override
    {
        _label = std::make_unique<Label>(_window->get());
        _label->align(LV_ALIGN_CENTER, 0, 0); _label->setTextFont(&lv_font_montserrat_28);
        _label->setTextColor(lv_color_hex(COL_LABEL)); _label->setText(_body);
    }
    void onClose() override { _label.reset(); }
private:
    std::string _body;
    std::unique_ptr<Label> _label;
};

/* --------------------------- ventana firmwares ---------------------------- */
class FirmwareWindow : public ui::Window {
public:
    FirmwareWindow()
    {
        config.title = "LAUNCHER  -  Firmwares (.bin)"; config.bgColor = COL_CARD;
        config.borderColor = COL_BORDER; config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 860, 480, 255};
    }
    void onOpen() override
    {
        _list = std::make_unique<Container>(_window->get());
        _list->align(LV_ALIGN_CENTER, 0, 24); _list->setSize(800, 360);
        _list->setBorderWidth(0); _list->setBgColor(lv_color_hex(0x0a120f));
        _msg = std::make_unique<Label>(_window->get());
        _msg->align(LV_ALIGN_CENTER, 0, -16); _msg->setTextFont(&lv_font_montserrat_24);
        _msg->setTextColor(lv_color_hex(COL_LABEL)); _msg->setText("Scanning SD for firmwares ...");
    }
    void onUpdate() override
    {
        // Progreso de flasheo (poll). Aparece la barra al pulsar un firmware.
        int fp = GetHAL()->flashProgress();
        if (fp >= 0 || fp == -2) {
            if (!_bar) {
                _bar = lv_bar_create(_window->get());
                lv_obj_set_size(_bar, 700, 26); lv_obj_align(_bar, LV_ALIGN_CENTER, 0, 40);
                lv_obj_set_style_bg_color(_bar, lv_color_hex(0x0a120f), LV_PART_MAIN);
                lv_obj_set_style_bg_color(_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
                _flashMsg = std::make_unique<Label>(_window->get());
                _flashMsg->align(LV_ALIGN_CENTER, 0, 0); _flashMsg->setTextFont(&lv_font_montserrat_24);
                _flashMsg->setTextColor(lv_color_hex(COL_ACCENT));
            }
            if (fp == -2) { lv_bar_set_value(_bar, 0, LV_ANIM_OFF); _flashMsg->setText(LV_SYMBOL_WARNING "  Flash failed (incompatible?)"); }
            else {
                lv_bar_set_value(_bar, fp, LV_ANIM_OFF);
                char b[40]; snprintf(b, sizeof(b), "Flashing... %d%%  (do not power off)", fp);
                _flashMsg->setText(b);
            }
        }

        if (_state != Opened || _scanned) return;
        _scanned = true;
        std::string lastFw = GetHAL()->getSettingStr("last_fw", "");
        auto entries = GetHAL()->scanSdCard("/");
        int n = 0;
        for (auto& e : entries) {
            if (e.isDir || !is_bin(e.name)) continue;
            std::string name = e.name;
            bool isLast = (name == lastFw);
            auto row = std::make_unique<Container>(_list->get());
            row->align(LV_ALIGN_TOP_LEFT, 0, n * 46); row->setSize(770, 42);
            row->setBgColor(lv_color_hex(isLast ? 0x1f4a3a : 0x12241c)); row->setBorderWidth(0);
            row->onClick().connect([this, name] {
                audio::play_next_tone_progression();
                GetHAL()->bootFirmwareAsync(std::string("/sd/") + name);  // progreso en onUpdate
            });
            auto lbl = std::make_unique<Label>(row->get());
            lbl->align(LV_ALIGN_LEFT_MID, 12, 0); lbl->setTextFont(&lv_font_montserrat_24);
            lbl->setTextColor(lv_color_hex(COL_ACCENT));
            lbl->setText(std::string(LV_SYMBOL_PLAY "  ") + name + (isLast ? "   " LV_SYMBOL_REFRESH " last" : ""));
            _rows.push_back(std::move(row)); _rows_lbl.push_back(std::move(lbl));
            n++;
        }
        if (n == 0) _msg->setText(GetHAL()->isSdCardMounted()
                                  ? "No .bin firmwares in SD root."
                                  : "Insert an SD card with .bin firmwares.");
        else _msg.reset();
    }
    void onClose() override
    {
        _rows_lbl.clear(); _rows.clear(); _msg.reset(); _list.reset();
        _flashMsg.reset();
        if (_bar) { lv_obj_del(_bar); _bar = nullptr; }
    }
private:
    bool _scanned = false;
    std::unique_ptr<Container> _list;
    std::unique_ptr<Label> _msg, _flashMsg;
    std::vector<std::unique_ptr<Container>> _rows;
    std::vector<std::unique_ptr<Label>> _rows_lbl;
    lv_obj_t* _bar = nullptr;
};

/* ----------------------- ventana de control (brillo/vol) ------------------ */
static void ctrl_step_cb(lv_event_t* e);  // fwd
class ControlWindow : public ui::Window {
public:
    ControlWindow(bool brightness) : _bright(brightness)
    {
        config.title = brightness ? "LCD BRIGHTNESS" : "SPEAKER VOLUME";
        config.bgColor = COL_CARD; config.borderColor = COL_BORDER; config.titleColor = COL_TITLE;
        config.closeBtn = true;
        config.kfClosed = {0, 0, 160, 120, 0}; config.kfOpened = {0, 0, 520, 320, 255};
    }
    bool isBright() const { return _bright; }
    void onOpen() override
    {
        _value = std::make_unique<Label>(_window->get());
        _value->align(LV_ALIGN_CENTER, 0, -20); _value->setTextFont(&lv_font_montserrat_44);
        _value->setTextColor(lv_color_hex(COL_VALUE)); _value->setText("--");

        _minus = std::make_unique<Container>(_window->get());
        make_btn(_minus.get(), "-", -10);
        _minus->align(LV_ALIGN_CENTER, -110, 70);
        _plus = std::make_unique<Container>(_window->get());
        make_btn(_plus.get(), "+", +10);
        _plus->align(LV_ALIGN_CENTER, 110, 70);
    }
    void onUpdate() override
    {
        if (_state != Opened || !_value) return;
        char buf[16];
        int v = _bright ? GetHAL()->getDisplayBrightness() : GetHAL()->getSpeakerVolume();
        snprintf(buf, sizeof(buf), "%d%s", v, _bright ? "%" : "");
        _value->setText(buf);
    }
    void onClose() override { _value.reset(); _minus.reset(); _plus.reset(); }
private:
    void make_btn(Container* c, const char* txt, int delta)
    {
        c->setSize(120, 90); c->setRadius(12); c->setBgColor(lv_color_hex(0x16382b));
        c->setBorderWidth(1);
        // code: 0=bright-, 1=bright+, 2=vol-, 3=vol+
        int code = (_bright ? 0 : 2) + (delta > 0 ? 1 : 0);
        lv_obj_add_event_cb(c->get(), ctrl_step_cb, LV_EVENT_CLICKED, (void*)(intptr_t)code);
        auto l = lv_label_create(c->get());
        lv_label_set_text(l, txt); lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_44, 0);
    }
    bool _bright;
    std::unique_ptr<Label> _value;
    std::unique_ptr<Container> _minus, _plus;
};
static void ctrl_step_cb(lv_event_t* e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);  // 0=br- 1=br+ 2=vol- 3=vol+
    bool bright = code < 2;
    int delta = (code & 1) ? +10 : -10;
    if (bright) {
        int v = (int)GetHAL()->getDisplayBrightness() + delta;
        if (v < 0) { v = 0; }
        if (v > 100) { v = 100; }
        GetHAL()->setDisplayBrightness((uint8_t)v);
    } else {
        int v = (int)GetHAL()->getSpeakerVolume() + delta;
        if (v < 0) { v = 0; }
        if (v > 100) { v = 100; }
        GetHAL()->setSpeakerVolume((uint8_t)v);
    }
}

/* --------------------- ventana IMU (cruceta + bola) ----------------------- */
class MotionWindow : public ui::Window {
public:
    MotionWindow()
    {
        config.title = "IMU / MOTION  -  Tilt"; config.bgColor = COL_CARD;
        config.borderColor = COL_BORDER; config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 560, 560, 255};
    }
    void onOpen() override
    {
        _area = lv_obj_create(_window->get());
        lv_obj_set_size(_area, AREA, AREA);
        lv_obj_align(_area, LV_ALIGN_CENTER, 0, 16);
        lv_obj_set_style_bg_color(_area, lv_color_hex(0x0a120f), 0);
        lv_obj_set_style_border_color(_area, lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_border_width(_area, 1, 0);
        lv_obj_set_style_radius(_area, 8, 0);
        lv_obj_clear_flag(_area, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* h = lv_obj_create(_area);
        lv_obj_set_size(h, AREA, 2); lv_obj_align(h, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(COL_BORDER), 0); lv_obj_set_style_border_width(h, 0, 0);
        lv_obj_t* v = lv_obj_create(_area);
        lv_obj_set_size(v, 2, AREA); lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(v, lv_color_hex(COL_BORDER), 0); lv_obj_set_style_border_width(v, 0, 0);

        _ball = lv_obj_create(_area);
        lv_obj_set_size(_ball, 46, 46);
        lv_obj_set_style_radius(_ball, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_ball, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(_ball, 0, 0);
        lv_obj_clear_flag(_ball, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(_ball, LV_ALIGN_CENTER, 0, 0);
    }
    void onUpdate() override
    {
        if (_state != Opened || !_ball) return;
        GetHAL()->updateImuData();
        int range = AREA / 2 - 26;
        int x = (int)(GetHAL()->imuData.accelX * range);
        int y = (int)(GetHAL()->imuData.accelY * range);
        if (x < -range) { x = -range; }
        if (x > range)  { x = range; }
        if (y < -range) { y = -range; }
        if (y > range)  { y = range; }
        lv_obj_align(_ball, LV_ALIGN_CENTER, x, y);
    }
    void onClose() override { if (_area) { lv_obj_del(_area); _area = nullptr; _ball = nullptr; } }
private:
    static const int AREA = 440;
    lv_obj_t *_area = nullptr, *_ball = nullptr;
};

/* --------------------------- ventana I2C scan ----------------------------- */
class I2cWindow : public ui::Window {
public:
    I2cWindow()
    {
        config.title = "I2C SCAN"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 780, 440, 255};
    }
    void onOpen() override
    {
        _msg = std::make_unique<Label>(_window->get());
        _msg->align(LV_ALIGN_CENTER, 0, 0); _msg->setTextFont(&lv_font_montserrat_24);
        _msg->setTextColor(lv_color_hex(COL_LABEL));
        _msg->setText("Scanning internal I2C bus ...");
    }
    void onUpdate() override
    {
        if (_state != Opened || _done) return;
        _done = true;
        auto fmt = [](const std::vector<uint8_t>& a) {
            if (a.empty()) return std::string("(none)");
            std::string s;
            char b[8];
            for (size_t i = 0; i < a.size(); i++) {
                snprintf(b, sizeof(b), "0x%02X  ", a[i]);
                s += b;
            }
            return s;
        };
        std::string s = "Internal bus:\n  " + fmt(GetHAL()->i2cScan(true));
        s += "\n\nExternal (ext.port1):\n  " + fmt(GetHAL()->i2cScan(false));
        s += "\n\n(keyboard TCA8418 = 0x34)";
        _msg->setText(s.c_str());
    }
    void onClose() override { _msg.reset(); }
private:
    bool _done = false;
    std::unique_ptr<Label> _msg;
};

/* ------------------------- ventana File Browser --------------------------- */
// Portapapeles del file browser (persiste entre aperturas de la ventana).
static std::string s_clip_path;   // ruta completa "/sd/..." copiada/cortada
static bool        s_clip_cut = false;

class FileBrowserWindow : public ui::Window {
public:
    FileBrowserWindow()
    {
        config.title = "FILE BROWSER"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 1120, 650, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        // Ruta actual
        _path_lbl = std::make_unique<Label>(root);
        _path_lbl->align(LV_ALIGN_TOP_LEFT, 24, 56); _path_lbl->setTextFont(&lv_font_montserrat_20);
        _path_lbl->setTextColor(lv_color_hex(COL_TITLE));
        // Lista (flex column, scrollable)
        _list = std::make_unique<Container>(root);
        _list->align(LV_ALIGN_TOP_LEFT, 24, 88); _list->setSize(1072, 430);
        _list->setBorderWidth(0); _list->setBgColor(lv_color_hex(0x0a120f)); _list->setRadius(8);
        lv_obj_t* lo = _list->get();
        lv_obj_set_flex_flow(lo, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(lo, 2, 0);
        lv_obj_set_style_pad_all(lo, 6, 0);
        lv_obj_set_scroll_dir(lo, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(lo, LV_SCROLLBAR_MODE_AUTO);
        // Barra de acciones (8 botones)
        int bx = 24; const int step = 134;
        addBtn(root, LV_SYMBOL_EDIT  " Open", bx, 0x43D2FF, [this]{ actOpen(); });    bx += step;
        addBtn(root, LV_SYMBOL_PLUS  " File", bx, 0x38EF7D, [this]{ actNewFile(); });  bx += step;
        addBtn(root, LV_SYMBOL_DIRECTORY " Dir", bx, 0xFDBE1A, [this]{ actNewDir(); }); bx += step;
        addBtn(root, LV_SYMBOL_COPY  " Copy", bx, 0xC0C8C4, [this]{ actClip(false); }); bx += step;
        addBtn(root, LV_SYMBOL_CUT   " Cut",  bx, 0xC0C8C4, [this]{ actClip(true); });  bx += step;
        addBtn(root, LV_SYMBOL_PASTE " Paste",bx, 0xC0C8C4, [this]{ actPaste(); });     bx += step;
        addBtn(root, LV_SYMBOL_LOOP  " Ren",  bx, 0xFDBE1A, [this]{ actRename(); });    bx += step;
        addBtn(root, LV_SYMBOL_TRASH " Del",  bx, 0xFF6B6B, [this]{ actDelete(); });
        // Linea de estado (sobre la barra de botones)
        _msg = std::make_unique<Label>(root);
        _msg->align(LV_ALIGN_BOTTOM_LEFT, 24, -70); _msg->setTextFont(&lv_font_montserrat_20);
        _msg->setTextColor(lv_color_hex(COL_LABEL)); _msg->setText("");
        _dirty = true;
    }
    void onUpdate() override
    {
        if (_state != Opened) return;
        if (_startRename) { _startRename = false; beginRename(); }  // fuera del evento
        if (_renameAction) finishRename();   // procesa fuera del evento (seguro)
        if (_dirty) { _dirty = false; rebuild(); }
    }
    void onClose() override
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        if (_renameTa) { lv_obj_del(_renameTa); _renameTa = nullptr; }
        _rows.clear(); _rowLabels.clear(); _btnLabels.clear();
        _msg.reset(); _list.reset(); _path_lbl.reset(); _btns.clear();
    }

private:
    // Ruta completa de un nombre dentro del cwd actual.
    std::string full(const std::string& name) const
    {
        std::string p = "/sd";
        if (!_cwd.empty()) p += "/" + _cwd;
        return p + "/" + name;
    }
    void setStatus(const std::string& s) { if (_msg) _msg->setText(s); }

    void addBtn(lv_obj_t* parent, const std::string& txt, int x, uint32_t color,
                std::function<void()> cb)
    {
        auto b = std::make_unique<Container>(parent);
        b->setSize(btn_w(txt), 44); b->align(LV_ALIGN_BOTTOM_LEFT, x, -16);
        b->setRadius(8); b->setBgColor(lv_color_hex(0x16241d)); b->setBorderWidth(1);
        b->setBorderColor(lv_color_hex(0x2a3a32));
        auto l = std::make_unique<Label>(b->get());
        l->align(LV_ALIGN_CENTER, 0, 0); l->setTextFont(&lv_font_montserrat_20);
        l->setTextColor(lv_color_hex(color)); l->setText(txt);
        b->onClick().connect(cb);
        _btnLabels.push_back(std::move(l));
        _btns.push_back(std::move(b));
    }
    static int btn_w(const std::string&) { return 124; }

    void rebuild()
    {
        _rows.clear(); _rowLabels.clear();
        std::string title = "/sd" + (_cwd.empty() ? "" : "/" + _cwd);
        if (_path_lbl) _path_lbl->setText(title);

        auto entries = GetHAL()->scanSdCard(_cwd.empty() ? "/" : _cwd);
        // Orden: carpetas primero, luego ficheros (alfabetico simple no garantizado por el FS).
        std::vector<hal::HalBase::FileEntry_t> dirs, files;
        for (auto& e : entries) (e.isDir ? dirs : files).push_back(e);

        // ".." para subir
        if (!_cwd.empty()) addRow("..", true, false, true);
        for (auto& e : dirs)  addRow(e.name, true, false, false);
        for (auto& e : files) addRow(e.name, false, is_text(e.name), false);

        if (entries.empty())
            setStatus(GetHAL()->isSdCardMounted() ? "Empty folder." : "Insert an SD card.");
        rebindGroup();
    }

    // (Re)crea el grupo de navegacion por teclado: filas + botones de la barra.
    void rebindGroup()
    {
        if (_renaming) return;  // durante el rename el teclado va al textarea
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _grp = lv_group_create();
        lv_group_set_wrap(_grp, true);
        for (auto& r : _rows) {
            lv_group_add_obj(_grp, r->get());
            lv_obj_add_event_cb(r->get(), browser_nav_cb, LV_EVENT_KEY, this);
        }
        for (auto& b : _btns) {
            lv_group_add_obj(_grp, b->get());
            lv_obj_add_event_cb(b->get(), browser_nav_cb, LV_EVENT_KEY, this);
        }
        if (!_rows.empty()) lv_group_focus_obj(_rows[0]->get());
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);
    }

    // Cruceta: arriba/izda -> anterior, abajo/dcha -> siguiente (lista lineal).
    static void browser_nav_cb(lv_event_t* e)
    {
        auto* self = static_cast<FileBrowserWindow*>(lv_event_get_user_data(e));
        if (!self || !self->_grp) return;
        switch (lv_event_get_key(e)) {
            case LV_KEY_UP: case LV_KEY_LEFT:    lv_group_focus_prev(self->_grp); break;
            case LV_KEY_DOWN: case LV_KEY_RIGHT: lv_group_focus_next(self->_grp); break;
            default: break;
        }
    }

    void addRow(const std::string& name, bool isDir, bool txt, bool isUp)
    {
        bool selected = (!isUp && name == _sel);
        auto row = std::make_unique<Container>(_list->get());
        row->setSize(LV_PCT(100), 42); row->setRadius(6); row->setBorderWidth(0);
        row->setBgColor(lv_color_hex(selected ? 0x1f4a3a : 0x0a120f));
        auto l = std::make_unique<Label>(row->get());
        l->align(LV_ALIGN_LEFT_MID, 10, 0); l->setTextFont(&lv_font_montserrat_24);
        uint32_t c = isUp ? 0xC0C8C4 : (isDir ? 0xFDBE1A : (txt ? 0x38EF7D : 0x43D2FF));
        l->setTextColor(lv_color_hex(c));
        l->setText(std::string(isUp ? LV_SYMBOL_LEFT "  .."
                              : isDir ? std::string(LV_SYMBOL_DIRECTORY "  ") + name
                              : (txt ? std::string(LV_SYMBOL_EDIT "  ") + name
                                     : std::string(LV_SYMBOL_FILE "  ") + name)));
        std::string nm = name;
        if (isUp) {
            row->onClick().connect([this]{ goUp(); });
        } else if (isDir) {
            row->onClick().connect([this, nm]{ enterDir(nm); });
        } else {
            bool t = txt;
            row->onClick().connect([this, nm, t]{ selectFile(nm); });
        }
        _rowLabels.push_back(std::move(l));
        _rows.push_back(std::move(row));
    }

    void enterDir(const std::string& name) { _cwd = _cwd.empty() ? name : _cwd + "/" + name; _sel.clear(); _dirty = true; }
    void goUp()
    {
        auto p = _cwd.find_last_of('/');
        _cwd = (p == std::string::npos) ? "" : _cwd.substr(0, p);
        _sel.clear(); _dirty = true;
    }
    void selectFile(const std::string& name) { _sel = name; _dirty = true; }

    void actOpen()
    {
        if (_sel.empty()) { setStatus("Select a file first."); return; }
        if (is_text(_sel) && s_view) s_view->request_edit(full(_sel));
        else setStatus("Not a text file.");
    }
    std::string uniqueName(const std::string& base, const std::string& ext)
    {
        auto entries = GetHAL()->scanSdCard(_cwd.empty() ? "/" : _cwd);
        for (int i = 0; i < 100; i++) {
            std::string cand = base + (i ? std::to_string(i) : "") + ext;
            bool exists = false;
            for (auto& e : entries) if (e.name == cand) { exists = true; break; }
            if (!exists) return cand;
        }
        return base + "_x" + ext;
    }
    void actNewFile()
    {
        std::string n = uniqueName("new_file", ".txt");
        if (GetHAL()->createFile(full(n))) { _sel = n; setStatus("Created " + n); }
        else setStatus("Create failed.");
        _dirty = true;
    }
    void actNewDir()
    {
        std::string n = uniqueName("new_folder", "");
        if (GetHAL()->makeDir(full(n))) setStatus("Created " + n);
        else setStatus("Mkdir failed.");
        _dirty = true;
    }
    void actClip(bool cut)
    {
        if (_sel.empty()) { setStatus("Select a file first."); return; }
        s_clip_path = full(_sel); s_clip_cut = cut;
        setStatus(std::string(cut ? "Cut: " : "Copied: ") + _sel);
    }
    void actPaste()
    {
        if (s_clip_path.empty()) { setStatus("Clipboard empty."); return; }
        auto p = s_clip_path.find_last_of('/');
        std::string name = (p == std::string::npos) ? s_clip_path : s_clip_path.substr(p + 1);
        std::string dst = full(name);
        bool ok = s_clip_cut ? GetHAL()->movePath(s_clip_path, dst)
                             : GetHAL()->copyFile(s_clip_path, dst);
        setStatus(ok ? (s_clip_cut ? "Moved." : "Pasted.") : "Paste failed.");
        if (ok && s_clip_cut) s_clip_path.clear();
        _dirty = true;
    }
    void actDelete()
    {
        if (_sel.empty()) { setStatus("Select a file first."); return; }
        bool ok = GetHAL()->deletePath(full(_sel));
        setStatus(ok ? "Deleted." : "Delete failed.");
        if (ok) _sel.clear();
        _dirty = true;
    }

    // ---- Rename: overlay con textarea + teclado ----
    // Solo marca; el overlay y el cambio de grupo se crean en onUpdate (fuera del
    // evento) para no borrar el grupo del teclado mientras el indev procesa ENTER.
    void actRename()
    {
        if (_sel.empty()) { setStatus("Select a file first."); return; }
        if (_renaming) return;
        _startRename = true;
    }
    void beginRename()
    {
        _renaming = true; _renameAction = 0;
        _renameTa = lv_textarea_create(_window->get());
        lv_textarea_set_one_line(_renameTa, true);
        lv_textarea_set_text(_renameTa, _sel.c_str());
        lv_obj_set_size(_renameTa, 760, 58);
        lv_obj_align(_renameTa, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_font(_renameTa, &lv_font_montserrat_24, 0);
        lv_obj_set_style_border_color(_renameTa, lv_color_hex(COL_ACCENT), 0);
        lv_obj_add_event_cb(_renameTa, rename_ready_cb, LV_EVENT_READY, this);
        lv_obj_add_event_cb(_renameTa, rename_key_cb, LV_EVENT_KEY, this);
        setStatus("Rename: edit, Enter = OK, Esc = cancel");
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _grp = lv_group_create();
        lv_group_add_obj(_grp, _renameTa);
        lv_group_focus_obj(_renameTa);
        lv_group_set_editing(_grp, true);  // las teclas van al textarea
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);
    }
    static void rename_ready_cb(lv_event_t* e)
    { auto* s = static_cast<FileBrowserWindow*>(lv_event_get_user_data(e)); if (s) s->_renameAction = 1; }
    static void rename_key_cb(lv_event_t* e)
    {
        if (lv_event_get_key(e) == LV_KEY_ESC) {
            auto* s = static_cast<FileBrowserWindow*>(lv_event_get_user_data(e));
            if (s) s->_renameAction = 2;
        }
    }
    // Se ejecuta desde onUpdate (fuera del manejador de evento): seguro destruir objetos.
    void finishRename()
    {
        int act = _renameAction; _renameAction = 0;
        if (act == 1 && _renameTa) {
            std::string nn = lv_textarea_get_text(_renameTa);
            for (auto& c : nn) if (c == '/' || c == '\\') c = '_';
            if (!nn.empty() && nn != _sel && GetHAL()->movePath(full(_sel), full(nn))) {
                _sel = nn; setStatus("Renamed to " + nn);
            } else setStatus("Rename failed.");
        } else {
            setStatus("Rename cancelled.");
        }
        if (_renameTa) { lv_obj_del(_renameTa); _renameTa = nullptr; }
        _renaming = false;
        _dirty = true;  // rebuild + rebindGroup restauran la navegacion de lista
    }

    bool _dirty = false;
    std::string _cwd;   // ruta relativa dentro de /sd ("" = raiz)
    std::string _sel;   // nombre del fichero seleccionado en el cwd
    std::unique_ptr<Container> _list;
    std::unique_ptr<Label> _msg;
    std::unique_ptr<Label> _path_lbl;
    std::vector<std::unique_ptr<Container>> _rows;
    std::vector<std::unique_ptr<Label>> _rowLabels;
    std::vector<std::unique_ptr<Container>> _btns;
    std::vector<std::unique_ptr<Label>> _btnLabels;
    lv_group_t* _grp = nullptr;     // grupo de navegacion por teclado (lista + barra)
    bool _renaming = false;         // overlay de rename activo
    bool _startRename = false;      // peticion de abrir rename (diferida a onUpdate)
    int  _renameAction = 0;         // 0=nada, 1=confirmar, 2=cancelar (procesa onUpdate)
    lv_obj_t* _renameTa = nullptr;  // textarea de rename
};

/* --------------------------- editor de texto ------------------------------ */
class TextEditorWindow : public ui::Window {
public:
    TextEditorWindow(const std::string& path) : _path(path)
    {
        config.title = std::string("EDIT  -  ") + path;
        config.bgColor = COL_CARD; config.borderColor = COL_BORDER; config.titleColor = COL_TITLE;
        config.closeBtn = true; config.clickBgClose = false;  // no cerrar al tocar fuera mientras editas
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 940, 600, 255};
    }
    void onOpen() override
    {
        std::string content = GetHAL()->readTextFile(_path);
        _ta = lv_textarea_create(_window->get());
        lv_obj_set_size(_ta, 880, 430);
        lv_obj_align(_ta, LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_set_style_text_font(_ta, &lv_font_montserrat_20, 0);
        lv_obj_set_style_bg_color(_ta, lv_color_hex(0x0a120f), 0);
        lv_obj_set_style_text_color(_ta, lv_color_hex(COL_LABEL), 0);
        lv_obj_set_style_border_color(_ta, lv_color_hex(COL_BORDER), 0);
        lv_textarea_set_text(_ta, content.c_str());

        _group = lv_group_create();
        lv_group_add_obj(_group, _ta);
        lv_group_focus_obj(_ta);
        if (GetHAL()->lvKeyboard) { lv_indev_set_group(GetHAL()->lvKeyboard, _group); }

        _status = lv_label_create(_window->get());
        lv_obj_align(_status, LV_ALIGN_BOTTOM_LEFT, 30, -28);
        lv_obj_set_style_text_font(_status, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(_status, lv_color_hex(0x9be8b8), 0);
        lv_label_set_text(_status, GetHAL()->isKeyboardConnected() ? "Keyboard ready"
                                                                   : "Connect a USB keyboard to type");

        _save = std::make_unique<Container>(_window->get());
        _save->setSize(220, 60); _save->setRadius(10); _save->setBgColor(lv_color_hex(0x16382b));
        _save->setBorderWidth(1); _save->align(LV_ALIGN_BOTTOM_RIGHT, -30, -22);
        _save->onClick().connect([this] {
            bool ok = GetHAL()->writeTextFile(_path, lv_textarea_get_text(_ta));
            lv_label_set_text(_status, ok ? "Saved." : "Save failed.");
        });
        auto sl = lv_label_create(_save->get());
        lv_label_set_text(sl, LV_SYMBOL_SAVE "  Save"); lv_obj_center(sl);
        lv_obj_set_style_text_color(sl, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_24, 0);
    }
    void onClose() override
    {
        if (GetHAL()->lvKeyboard) { lv_indev_set_group(GetHAL()->lvKeyboard, nullptr); }
        _save.reset();
        if (_ta) { lv_obj_del(_ta); _ta = nullptr; }
        if (_status) { lv_obj_del(_status); _status = nullptr; }
        if (_group) { lv_group_del(_group); _group = nullptr; }
    }
private:
    std::string _path;
    lv_obj_t* _ta = nullptr;
    lv_obj_t* _status = nullptr;
    lv_group_t* _group = nullptr;
    std::unique_ptr<Container> _save;
};

/* ----------------------------- ventana GPIO ------------------------------- */
static void gpio_sw_cb(lv_event_t* e)
{
    int pin = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    GetHAL()->gpioSetLevel((uint8_t)pin, on);
}
class GpioWindow : public ui::Window {
public:
    GpioWindow()
    {
        config.title = "GPIO / IO TEST"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 760, 480, 255};
    }
    void onOpen() override
    {
        // Pines de cabecera seguros (sin 37/38 = consola UART).
        static const uint8_t pins[] = {18, 19, 5, 7, 3, 2, 47, 16};
        lv_obj_t* cont = _window->get();
        int n = sizeof(pins) / sizeof(pins[0]);
        for (int i = 0; i < n; i++) {
            GetHAL()->gpioReset(pins[i]);
            GetHAL()->gpioInitOutput(pins[i]);
            GetHAL()->gpioSetLevel(pins[i], false);

            int col = i % 2, row = i / 2;
            auto lab = std::make_unique<Label>(cont);
            lab->align(LV_ALIGN_TOP_LEFT, 60 + col * 360, 70 + row * 80);
            lab->setTextFont(&lv_font_montserrat_28); lab->setTextColor(lv_color_hex(COL_LABEL));
            lab->setText(std::string("GPIO ") + std::to_string(pins[i]));

            lv_obj_t* sw = lv_switch_create(cont);
            lv_obj_align(sw, LV_ALIGN_TOP_LEFT, 200 + col * 360, 64 + row * 80);
            lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, gpio_sw_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)pins[i]);
            _labels.push_back(std::move(lab));
            _sw.push_back(sw);
        }
    }
    void onClose() override { _labels.clear(); _sw.clear(); }
private:
    std::vector<std::unique_ptr<Label>> _labels;
    std::vector<lv_obj_t*> _sw;
};

/* ----------------------------- ventana Camera ----------------------------- */
class CameraWindow : public ui::Window {
public:
    CameraWindow()
    {
        config.title = "CAMERA"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 900, 560, 255};
    }
    void onOpen() override
    {
        // Canvas en la pantalla (no hijo de la ventana) para controlar su vida.
        _canvas = lv_canvas_create(lv_screen_active());
        lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 16);
        lv_obj_add_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    // Cierre diferido: primero para el task de camara, y SOLO cuando ha parado
    // (isCameraCapturing()==false) cerramos de verdad -> evita el crash.
    void close(bool teleport = false, bool triggerCallback = true) override
    {
        if (!_started) { ui::Window::close(teleport, triggerCallback); return; }
        if (!_closing) {
            _closing = true;
            if (_canvas) { lv_obj_add_flag(_canvas, LV_OBJ_FLAG_HIDDEN); }
            GetHAL()->stopCameraCapture();
        }
    }
    void onUpdate() override
    {
        if (_closing) {
            if (GetHAL()->isCameraCapturing()) { return; }  // espera a que pare el task
            // Cerrar de verdad UNA sola vez: si se llama cada frame, la animacion
            // de cierre se reinicia y el estado nunca llega a Closed -> _window
            // nunca se libera -> no se puede entrar en ningun tile (bug B2).
            _closing = false;
            ui::Window::close();
            return;
        }
        if (_state != Opened) { return; }
        if (!_started) {
            GetHAL()->startCameraCapture(_canvas);
            if (_canvas) { lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_HIDDEN); }
            _started = true;
        }
    }
    void onClose() override
    {
        if (_canvas) { lv_obj_del(_canvas); _canvas = nullptr; }  // task ya parado
    }
private:
    lv_obj_t* _canvas = nullptr;
    bool _started = false;
    bool _closing = false;
};

/* ----------------------------- ventana UART ------------------------------- */
class UartWindow : public ui::Window {
public:
    UartWindow()
    {
        config.title = "UART MONITOR"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 860, 480, 255};
    }
    void onOpen() override
    {
        _rx = lv_label_create(_window->get());
        lv_label_set_long_mode(_rx, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(_rx, 800, 320);
        lv_obj_align(_rx, LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_set_style_text_font(_rx, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(_rx, lv_color_hex(0x43d2ff), 0);
        lv_label_set_text(_rx, "Waiting for UART/RS485 data ...");

        _send = std::make_unique<Container>(_window->get());
        _send->setSize(260, 60); _send->setRadius(10); _send->setBgColor(lv_color_hex(0x16382b));
        _send->setBorderWidth(1); _send->align(LV_ALIGN_BOTTOM_MID, 0, -16);
        _send->onClick().connect([] { GetHAL()->uartMonitorSend("Hello from LOADOUT!"); });
        auto sl = lv_label_create(_send->get());
        lv_label_set_text(sl, "Send test"); lv_obj_center(sl);
        lv_obj_set_style_text_color(sl, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
    }
    void onUpdate() override
    {
        if (_state != Opened || !_rx) return;
        {
            std::lock_guard<std::mutex> lk(GetHAL()->uartMonitorData.mutex);
            while (!GetHAL()->uartMonitorData.rxQueue.empty()) {
                _buf += (char)GetHAL()->uartMonitorData.rxQueue.front();
                GetHAL()->uartMonitorData.rxQueue.pop();
            }
        }
        if (_buf.size() > 1500) { _buf = _buf.substr(_buf.size() - 1500); }
        if (!_buf.empty()) { lv_label_set_text(_rx, _buf.c_str()); }
    }
    void onClose() override { _send.reset(); if (_rx) { lv_obj_del(_rx); _rx = nullptr; } }
private:
    lv_obj_t* _rx = nullptr;
    std::unique_ptr<Container> _send;
    std::string _buf;
};

/* --------------------------- ventana de power ----------------------------- */
class PowerWindow : public ui::Window {
public:
    PowerWindow()
    {
        config.title = "POWER"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 160, 120, 0}; config.kfOpened = {0, 0, 560, 320, 255};
    }
    void onOpen() override
    {
        _off = std::make_unique<Container>(_window->get());
        mkbtn(_off.get(), LV_SYMBOL_POWER "  POWER OFF", 0xff5f57, -52);
        _off->onClick().connect([] { GetHAL()->powerOff(); });
        _slp = std::make_unique<Container>(_window->get());
        mkbtn(_slp.get(), "SLEEP", COL_ACCENT, 52);
        _slp->onClick().connect([] { GetHAL()->sleepAndRtcWakeup(); });
    }
    void onClose() override { _off.reset(); _slp.reset(); }
private:
    void mkbtn(Container* c, const char* txt, uint32_t col, int y)
    {
        c->setSize(420, 84); c->setRadius(12); c->setBgColor(lv_color_hex(0x16382b));
        c->setBorderWidth(1); c->align(LV_ALIGN_CENTER, 0, y);
        auto l = lv_label_create(c->get()); lv_label_set_text(l, txt); lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    }
    std::unique_ptr<Container> _off, _slp;
};

/* --------------------------- ventana de WiFi ------------------------------ */
class WifiWindow : public ui::Window {
public:
    WifiWindow()
    {
        config.title = "WIFI / NETWORK"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 780, 420, 255};
    }
    void onOpen() override
    {
        _info = std::make_unique<Label>(_window->get());
        _info->align(LV_ALIGN_CENTER, 0, -30);
        _info->setTextFont(&lv_font_montserrat_24);
        _info->setTextColor(lv_color_hex(COL_LABEL));
        _info->setText(std::string("Access Point active:\n\n"
                       "  SSID:  LOADOUT\n"
                       "  Pass:  ") + GetHAL()->getSettingStr("wifi_pass", "loadout1234") + "\n");
        _url = std::make_unique<Label>(_window->get());
        _url->align(LV_ALIGN_CENTER, 0, 96);
        _url->setTextFont(&lv_font_montserrat_28);
        _url->setTextColor(lv_color_hex(COL_ACCENT));
        _url->setText(LV_SYMBOL_WIFI "  http://192.168.4.1");
    }
    void onClose() override { _info.reset(); _url.reset(); }
private:
    std::unique_ptr<Label> _info, _url;
};

/* ---------------------------- ventana About ------------------------------- */
class AboutWindow : public ui::Window {
public:
    AboutWindow()
    {
        config.title = "ABOUT"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 720, 440, 255};
    }
    void onOpen() override
    {
        _logo = std::make_unique<Label>(_window->get());
        _logo->align(LV_ALIGN_TOP_MID, 0, 70); _logo->setTextFont(&lv_font_montserrat_44);
        _logo->setTextColor(lv_color_hex(COL_ACCENT)); _logo->setText("// LOADOUT");

        _ver = std::make_unique<Label>(_window->get());
        _ver->align(LV_ALIGN_TOP_MID, 0, 130); _ver->setTextFont(&lv_font_montserrat_24);
        _ver->setTextColor(lv_color_hex(COL_VALUE)); _ver->setText(std::string("v") + LOADOUT_VERSION);

        _by = std::make_unique<Label>(_window->get());
        _by->align(LV_ALIGN_CENTER, 0, 40); _by->setTextFont(&lv_font_montserrat_24);
        _by->setTextColor(lv_color_hex(COL_LABEL));
        _by->setText("developed by\nidiotsandwich.club");
        lv_obj_set_style_text_align(_by->get(), LV_TEXT_ALIGN_CENTER, 0);

        _mail = std::make_unique<Label>(_window->get());
        _mail->align(LV_ALIGN_BOTTOM_MID, 0, -28); _mail->setTextFont(&lv_font_montserrat_20);
        _mail->setTextColor(lv_color_hex(COL_ICON)); _mail->setText("hello@idiotsandwich.club");
    }
    void onClose() override { _logo.reset(); _ver.reset(); _by.reset(); _mail.reset(); }
private:
    std::unique_ptr<Label> _logo, _ver, _by, _mail;
};

/* --------------------------- ventana Settings ----------------------------- */
class SettingsWindow : public ui::Window {
public:
    SettingsWindow()
    {
        config.title = "SETTINGS"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 900, 700, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        // --- Theme ---
        _lab = std::make_unique<Label>(root);
        _lab->align(LV_ALIGN_TOP_LEFT, 30, 60); _lab->setTextFont(&lv_font_montserrat_24);
        _lab->setTextColor(lv_color_hex(COL_LABEL)); _lab->setText("Theme");

        for (int i = 0; i < THEME_COUNT; i++) {
            const Theme& th = THEMES[i];
            auto sw = std::make_unique<Container>(root);
            sw->setSize(180, 96); sw->setRadius(12); sw->setBgColor(lv_color_hex(th.card));
            sw->setBorderWidth(i == g_theme ? 4 : 2);
            sw->setBorderColor(lv_color_hex(i == g_theme ? th.accent : 0x2a3a32));
            sw->align(LV_ALIGN_TOP_LEFT, 30 + i * 196, 96);
            // muestra de acento dentro (NO clicable: deja pasar el click al swatch)
            auto dot = std::make_unique<Container>(sw->get());
            dot->setSize(28, 28); dot->setRadius(14); dot->setBgColor(lv_color_hex(th.accent));
            dot->setBorderWidth(0); dot->align(LV_ALIGN_LEFT_MID, 12, 0);
            lv_obj_clear_flag(dot->get(), LV_OBJ_FLAG_CLICKABLE);
            std::string sn = th.name;  // nombre corto que cabe en la tarjeta
            if (sn.rfind("Solarized ", 0) == 0) sn = "Sol. " + sn.substr(10);
            else if (sn == "Amber Mono") sn = "Amber";
            auto nm = std::make_unique<Label>(sw->get());
            nm->align(LV_ALIGN_LEFT_MID, 48, 0); nm->setTextFont(&lv_font_montserrat_18);
            nm->setTextColor(lv_color_hex(th.title)); nm->setText(sn);
            lv_obj_clear_flag(nm->get(), LV_OBJ_FLAG_CLICKABLE);
            int idx = i;
            sw->onClick().connect([this, idx] {
                apply_theme(idx);
                GetHAL()->setSettingInt("theme", idx);
                // Resaltar el seleccionado al instante (feedback en vivo).
                for (int k = 0; k < (int)_sw.size(); k++) {
                    _sw[k]->setBorderWidth(k == idx ? 4 : 2);
                    _sw[k]->setBorderColor(lv_color_hex(k == idx ? THEMES[idx].accent : 0x2a3a32));
                }
                if (_status) _status->setText(std::string("Theme: ") + THEMES[idx].name);
                if (s_view) s_view->requestThemeReload();  // repinta el home en vivo al cerrar
            });
            _dots.push_back(std::move(dot));
            _names.push_back(std::move(nm));
            _sw.push_back(std::move(sw));
        }

        // --- Toggle del fondo animado ---
        _bgBtn = std::make_unique<Container>(root);
        _bgBtn->setSize(360, 40); _bgBtn->align(LV_ALIGN_TOP_LEFT, 30, 198);
        _bgBtn->setRadius(8); _bgBtn->setBorderWidth(1);
        _bgBtn->setBorderColor(lv_color_hex(0x2a3a32)); _bgBtn->setBgColor(lv_color_hex(0x12241c));
        _bgLab = std::make_unique<Label>(_bgBtn->get());
        _bgLab->align(LV_ALIGN_CENTER, 0, 0); _bgLab->setTextFont(&lv_font_montserrat_20);
        refreshBgLab();
        _bgBtn->onClick().connect([this] {
            int v = GetHAL()->getSettingInt("bg_fx", 1) ? 0 : 1;
            GetHAL()->setSettingInt("bg_fx", v);
            refreshBgLab();
            if (_status) _status->setText(v ? "Background ON" : "Background OFF");
            if (s_view) s_view->requestThemeReload();  // aplica al cerrar Settings
        });

        // --- Home WiFi (STA) ---
        _wifiLab = std::make_unique<Label>(root);
        _wifiLab->align(LV_ALIGN_TOP_LEFT, 30, 230); _wifiLab->setTextFont(&lv_font_montserrat_24);
        _wifiLab->setTextColor(lv_color_hex(COL_LABEL)); _wifiLab->setText("Home WiFi (internet)");
        _wifiStat = std::make_unique<Label>(root);
        _wifiStat->align(LV_ALIGN_TOP_LEFT, 30, 264); _wifiStat->setTextFont(&lv_font_montserrat_20);
        _wifiStat->setTextColor(lv_color_hex(COL_ACCENT)); _wifiStat->setText("...");
        _wifiHint = std::make_unique<Label>(root);
        _wifiHint->align(LV_ALIGN_TOP_LEFT, 30, 296); _wifiHint->setTextFont(&lv_font_montserrat_20);
        _wifiHint->setTextColor(lv_color_hex(0x7a8a82));
        _wifiHint->setText("Edit /sd/loadout.conf  ->  wifi_sta_ssid= / wifi_sta_pass= / theme=");

        // --- RTC clock: campo de texto editable con teclado (mas eficiente) ---
        _rtcLab = std::make_unique<Label>(root);
        _rtcLab->align(LV_ALIGN_TOP_LEFT, 30, 352); _rtcLab->setTextFont(&lv_font_montserrat_24);
        _rtcLab->setTextColor(lv_color_hex(COL_LABEL)); _rtcLab->setText("Clock (RTC)  -  type  YYYY-MM-DD HH:MM");
        struct tm t = {};
        GetHAL()->getRtcTime(&t);
        if (t.tm_year < 124) { t.tm_year = 126; t.tm_mon = 0; t.tm_mday = 1; }
        char cur[48];
        snprintf(cur, sizeof(cur), "%04d-%02d-%02d %02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        _rtcTa = lv_textarea_create(root);
        lv_textarea_set_one_line(_rtcTa, true);
        lv_textarea_set_text(_rtcTa, cur);
        lv_obj_set_size(_rtcTa, 470, 56); lv_obj_align(_rtcTa, LV_ALIGN_TOP_LEFT, 30, 392);
        lv_obj_set_style_text_font(_rtcTa, &lv_font_montserrat_28, 0);
        lv_obj_set_style_border_color(_rtcTa, lv_color_hex(COL_ACCENT), 0);
        lv_obj_add_event_cb(_rtcTa, rtc_ready_cb, LV_EVENT_READY, this);  // Enter = SET
        _apply = std::make_unique<Container>(root);
        _apply->setSize(180, 56); _apply->align(LV_ALIGN_TOP_LEFT, 520, 392);
        _apply->setRadius(8); _apply->setBgColor(lv_color_hex(0x16382b)); _apply->setBorderWidth(1);
        _apply->setBorderColor(lv_color_hex(COL_ACCENT));
        _applyLab = std::make_unique<Label>(_apply->get());
        _applyLab->align(LV_ALIGN_CENTER, 0, 0); _applyLab->setTextFont(&lv_font_montserrat_24);
        _applyLab->setTextColor(lv_color_hex(COL_ACCENT)); _applyLab->setText(LV_SYMBOL_OK " SET");
        _apply->onClick().connect([this] { applyRtc(); });

        // --- Lock PIN: bloqueo de arranque (4-8 digitos; vacio = sin bloqueo) ---
        _pinLab = std::make_unique<Label>(root);
        _pinLab->align(LV_ALIGN_TOP_LEFT, 30, 470); _pinLab->setTextFont(&lv_font_montserrat_24);
        _pinLab->setTextColor(lv_color_hex(COL_LABEL));
        _pinLab->setText("Lock PIN  -  4-8 digits  (empty = off)");
        _pinTa = lv_textarea_create(root);
        lv_textarea_set_one_line(_pinTa, true);
        lv_textarea_set_password_mode(_pinTa, true);
        lv_textarea_set_accepted_chars(_pinTa, "0123456789");
        lv_textarea_set_max_length(_pinTa, 8);
        lv_textarea_set_text(_pinTa, GetHAL()->getSettingStr("pin", "").c_str());
        lv_obj_set_size(_pinTa, 280, 56); lv_obj_align(_pinTa, LV_ALIGN_TOP_LEFT, 30, 510);
        lv_obj_set_style_text_font(_pinTa, &lv_font_montserrat_28, 0);
        lv_obj_set_style_border_color(_pinTa, lv_color_hex(COL_ACCENT), 0);
        lv_obj_add_event_cb(_pinTa, ta_focus_cb, LV_EVENT_CLICKED, this);
        _pinBtn = std::make_unique<Container>(root);
        _pinBtn->setSize(180, 56); _pinBtn->align(LV_ALIGN_TOP_LEFT, 330, 510);
        _pinBtn->setRadius(8); _pinBtn->setBgColor(lv_color_hex(0x16382b)); _pinBtn->setBorderWidth(1);
        _pinBtn->setBorderColor(lv_color_hex(COL_ACCENT));
        _pinBtnLab = std::make_unique<Label>(_pinBtn->get());
        _pinBtnLab->align(LV_ALIGN_CENTER, 0, 0); _pinBtnLab->setTextFont(&lv_font_montserrat_24);
        _pinBtnLab->setTextColor(lv_color_hex(COL_ACCENT)); _pinBtnLab->setText(LV_SYMBOL_OK " SET");
        _pinBtn->onClick().connect([this] { applyPin(); });
        _pinStat = std::make_unique<Label>(root);
        _pinStat->align(LV_ALIGN_TOP_LEFT, 530, 524); _pinStat->setTextFont(&lv_font_montserrat_20);
        refreshPinStat();

        // Teclado -> campos editables (RTC + PIN). Toca un campo para enfocarlo.
        _grp = lv_group_create();
        lv_group_add_obj(_grp, _rtcTa);
        lv_group_add_obj(_grp, _pinTa);
        lv_obj_add_event_cb(_rtcTa, ta_focus_cb, LV_EVENT_CLICKED, this);
        lv_group_focus_obj(_rtcTa);
        lv_group_set_editing(_grp, true);
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);

        _status = std::make_unique<Label>(root);
        _status->align(LV_ALIGN_BOTTOM_MID, 0, -16); _status->setTextFont(&lv_font_montserrat_20);
        _status->setTextColor(lv_color_hex(0x9be8b8)); _status->setText("Theme / WiFi / Clock");
    }
    void onUpdate() override
    {
        if (_state != Opened || !_wifiStat) return;
        bool conn = GetHAL()->wifiStaConnected();
        _wifiStat->setText(std::string(conn ? LV_SYMBOL_WIFI "  " : LV_SYMBOL_CLOSE "  ")
                           + GetHAL()->wifiStaInfo());
        _wifiStat->setTextColor(lv_color_hex(conn ? COL_ACCENT : 0xff8a80));
    }
    void onClose() override
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        if (_rtcTa) { lv_obj_del(_rtcTa); _rtcTa = nullptr; }
        if (_pinTa) { lv_obj_del(_pinTa); _pinTa = nullptr; }
        _lab.reset(); _dots.clear(); _names.clear(); _sw.clear(); _status.reset();
        _wifiLab.reset(); _wifiStat.reset(); _wifiHint.reset();
        _rtcLab.reset(); _apply.reset(); _applyLab.reset();
        _bgBtn.reset(); _bgLab.reset();
        _pinLab.reset(); _pinBtn.reset(); _pinBtnLab.reset(); _pinStat.reset();
    }
private:
    void refreshPinStat()
    {
        bool on = GetHAL()->getSettingStr("pin", "").size() >= 4;
        _pinStat->setText(on ? LV_SYMBOL_OK "  Lock ON" : "Lock OFF");
        _pinStat->setTextColor(lv_color_hex(on ? COL_ACCENT : 0x7a8a82));
    }
    void applyPin()
    {
        if (!_pinTa) return;
        std::string p = lv_textarea_get_text(_pinTa);
        if (p.empty()) {
            GetHAL()->setSettingStr("pin", "");
            if (_status) _status->setText("Lock disabled");
        } else if (p.size() >= 4 && p.size() <= 8) {
            GetHAL()->setSettingStr("pin", p);
            if (_status) _status->setText("PIN saved - active on next boot");
        } else if (_status) {
            _status->setText("PIN must be 4-8 digits");
        }
        refreshPinStat();
    }
    static void ta_focus_cb(lv_event_t* e)
    {
        auto* self = static_cast<SettingsWindow*>(lv_event_get_user_data(e));
        lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
        if (self && self->_grp && ta) { lv_group_focus_obj(ta); lv_group_set_editing(self->_grp, true); }
    }
    void refreshBgLab()
    {
        bool on = GetHAL()->getSettingInt("bg_fx", 1);
        _bgLab->setText(on ? LV_SYMBOL_EYE_OPEN "  Animated background: ON"
                           : LV_SYMBOL_EYE_CLOSE "  Animated background: OFF");
        _bgLab->setTextColor(lv_color_hex(on ? COL_ACCENT : 0x7a8a82));
    }
    void applyRtc()
    {
        if (!_rtcTa) return;
        std::string s = lv_textarea_get_text(_rtcTa);
        int Y = 0, Mo = 0, D = 0, H = 0, Mi = 0;
        if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &Y, &Mo, &D, &H, &Mi) == 5 &&
            Y >= 2024 && Mo >= 1 && Mo <= 12 && D >= 1 && D <= 31 && H < 24 && Mi < 60) {
            struct tm t = {};
            t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D;
            t.tm_hour = H; t.tm_min = Mi; t.tm_sec = 0;
            GetHAL()->setRtcTime(t);
            if (_status) _status->setText("Clock set");
        } else if (_status) {
            _status->setText("Format: YYYY-MM-DD HH:MM (year>=2024)");
        }
    }
    static void rtc_ready_cb(lv_event_t* e)
    { auto* s = static_cast<SettingsWindow*>(lv_event_get_user_data(e)); if (s) s->applyRtc(); }

    std::unique_ptr<Label> _lab, _status, _wifiLab, _wifiStat, _wifiHint, _rtcLab, _applyLab, _bgLab;
    std::unique_ptr<Label> _pinLab, _pinBtnLab, _pinStat;
    std::unique_ptr<Container> _bgBtn, _apply, _pinBtn;
    std::vector<std::unique_ptr<Container>> _sw, _dots;
    std::vector<std::unique_ptr<Label>> _names;
    lv_obj_t* _rtcTa = nullptr;
    lv_obj_t* _pinTa = nullptr;
    lv_group_t* _grp = nullptr;
};

/* ----------------------------- ventana OTA -------------------------------- */
class OtaWindow : public ui::Window {
public:
    OtaWindow()
    {
        config.title = "OTA UPDATE"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 820, 480, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        _cur = std::make_unique<Label>(root);
        _cur->align(LV_ALIGN_TOP_LEFT, 30, 60); _cur->setTextFont(&lv_font_montserrat_24);
        _cur->setTextColor(lv_color_hex(COL_LABEL));
        _cur->setText(std::string("Current: v") + LOADOUT_VERSION);

        _net = std::make_unique<Label>(root);
        _net->align(LV_ALIGN_TOP_LEFT, 30, 96); _net->setTextFont(&lv_font_montserrat_20);
        _net->setTextColor(lv_color_hex(0x7a8a82)); _net->setText("...");

        _info = std::make_unique<Label>(root);
        _info->align(LV_ALIGN_CENTER, 0, -10); _info->setTextFont(&lv_font_montserrat_24);
        _info->setTextColor(lv_color_hex(COL_ACCENT)); _info->setText("Tap CHECK to look for updates");
        lv_obj_set_width(_info->get(), 740);
        lv_label_set_long_mode(_info->get(), LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(_info->get(), LV_TEXT_ALIGN_CENTER, 0);

        _check = make_btn(root, LV_SYMBOL_REFRESH " CHECK", -150, 0x43d2ff, [this]{ GetHAL()->otaStartCheck(); });
        _update = make_btn(root, LV_SYMBOL_DOWNLOAD " UPDATE", 150, 0x38ef7d, [this]{ GetHAL()->otaStartUpdate(); });
        lv_obj_add_flag(_updateBtn, LV_OBJ_FLAG_HIDDEN);  // oculto hasta que haya update
    }
    void onUpdate() override
    {
        if (_state != Opened) return;
        if (_net) {
            bool c = GetHAL()->wifiStaConnected();
            _net->setText(std::string(c ? LV_SYMBOL_WIFI "  " : LV_SYMBOL_CLOSE "  ") + GetHAL()->wifiStaInfo());
            _net->setTextColor(lv_color_hex(c ? 0x38ef7d : 0xff8a80));
        }
        int st = GetHAL()->otaState();
        std::string latest = GetHAL()->otaLatestVersion();
        std::string msg    = GetHAL()->otaMessage();
        bool showUpdate = false;
        std::string txt;
        switch (st) {
            case 1: txt = "Checking..."; break;
            case 2: {
                bool newer = !latest.empty() && latest != std::string(LOADOUT_VERSION);
                txt = (newer ? "Update available: v" + latest : "Up to date (v" + latest + ")");
                if (!msg.empty()) txt += "\n" + msg;
                showUpdate = newer;
                break;
            }
            case 3: txt = "Updating...\n" + msg; break;
            case 4: txt = msg.empty() ? "Error" : msg; break;
            default: txt = "Tap CHECK to look for updates"; break;
        }
        if (_info) _info->setText(txt.c_str());
        if (_updateBtn) {
            if (showUpdate) lv_obj_clear_flag(_updateBtn, LV_OBJ_FLAG_HIDDEN);
            else            lv_obj_add_flag(_updateBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    void onClose() override
    { _cur.reset(); _net.reset(); _info.reset(); _check.reset(); _update.reset(); _cl.reset(); _ul.reset(); }
private:
    std::unique_ptr<Container> make_btn(lv_obj_t* p, const std::string& t, int dx, uint32_t col,
                                        std::function<void()> cb)
    {
        auto b = std::make_unique<Container>(p);
        b->setSize(240, 56); b->align(LV_ALIGN_BOTTOM_MID, dx, -24);
        b->setRadius(8); b->setBgColor(lv_color_hex(0x16241d)); b->setBorderWidth(1);
        b->setBorderColor(lv_color_hex(0x2a3a32));
        auto l = std::make_unique<Label>(b->get());
        l->align(LV_ALIGN_CENTER, 0, 0); l->setTextFont(&lv_font_montserrat_24);
        l->setTextColor(lv_color_hex(col)); l->setText(t);
        b->onClick().connect(cb);
        if (dx < 0) { _cl = std::move(l); } else { _ul = std::move(l); _updateBtn = b->get(); }
        return b;
    }
    std::unique_ptr<Label> _cur, _net, _info, _cl, _ul;
    std::unique_ptr<Container> _check, _update;
    lv_obj_t* _updateBtn = nullptr;
};

/* ---------------------------- ventana Chat IA ----------------------------- */
class ChatWindow : public ui::Window {
public:
    ChatWindow()
    {
        config.title = "ASK AI"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 1080, 660, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        // Conversacion (scroll vertical)
        _conv = std::make_unique<Container>(root);
        _conv->align(LV_ALIGN_TOP_MID, 0, 54); _conv->setSize(1020, 470);
        _conv->setBorderWidth(0); _conv->setBgColor(lv_color_hex(0x0a120f)); _conv->setRadius(8);
        lv_obj_set_flex_flow(_conv->get(), LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(_conv->get(), 8, 0); lv_obj_set_style_pad_all(_conv->get(), 12, 0);
        lv_obj_set_scroll_dir(_conv->get(), LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_conv->get(), LV_SCROLLBAR_MODE_AUTO);
        // Input
        _ta = lv_textarea_create(root);
        lv_textarea_set_one_line(_ta, true);
        lv_textarea_set_placeholder_text(_ta, "Ask something...");
        lv_obj_set_size(_ta, 840, 56); lv_obj_align(_ta, LV_ALIGN_BOTTOM_LEFT, 24, -18);
        lv_obj_set_style_text_font(_ta, &lv_font_montserrat_24, 0);
        lv_obj_set_style_border_color(_ta, lv_color_hex(COL_ACCENT), 0);
        lv_obj_add_event_cb(_ta, ready_cb, LV_EVENT_READY, this);
        // Send
        _send = std::make_unique<Container>(root);
        _send->setSize(170, 56); _send->align(LV_ALIGN_BOTTOM_RIGHT, -24, -18);
        _send->setRadius(8); _send->setBgColor(lv_color_hex(0x16382b)); _send->setBorderWidth(1);
        _send->setBorderColor(lv_color_hex(COL_ACCENT));
        _sendLab = std::make_unique<Label>(_send->get());
        _sendLab->align(LV_ALIGN_CENTER, 0, 0); _sendLab->setTextFont(&lv_font_montserrat_24);
        _sendLab->setTextColor(lv_color_hex(COL_ACCENT)); _sendLab->setText(LV_SYMBOL_RIGHT " SEND");
        _send->onClick().connect([this] { _pendingSend = true; });

        addBubble("Configure ai_url/ai_key/ai_model in /sd/loadout.conf, connect home WiFi, and ask.", false);

        // Teclado -> textarea (modo edicion)
        _grp = lv_group_create();
        lv_group_add_obj(_grp, _ta);
        lv_group_focus_obj(_ta);
        lv_group_set_editing(_grp, true);
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);
    }
    void onUpdate() override
    {
        if (_state != Opened) return;
        if (_pendingSend) {
            _pendingSend = false;
            std::string q = lv_textarea_get_text(_ta);
            // trim
            while (!q.empty() && (q.back() == ' ' || q.back() == '\n' || q.back() == '\r')) q.pop_back();
            if (!q.empty() && !_busy) {
                addBubble(q, true);
                lv_textarea_set_text(_ta, "");
                _pendBubble = addBubble("...", false);  // placeholder de respuesta
                GetHAL()->chatSend(q);
                _busy = true;
            }
        }
        if (_busy) {
            int st = GetHAL()->chatState();
            if (st == 1) { if (_pendBubble) lv_label_set_text(_pendBubble, "thinking..."); }
            else if (st == 2 || st == 3) {
                std::string r = GetHAL()->chatReply();
                if (_pendBubble) lv_label_set_text(_pendBubble, (st == 3 ? std::string(LV_SYMBOL_WARNING "  ") + r : r).c_str());
                _busy = false; _pendBubble = nullptr;
                GetHAL()->chatReset();
                if (_conv) lv_obj_scroll_to_y(_conv->get(), LV_COORD_MAX, LV_ANIM_OFF);
            }
        }
    }
    void onClose() override
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _bubbles.clear(); _bubbleLabels.clear(); _conv.reset(); _send.reset(); _sendLab.reset();
        if (_ta) { lv_obj_del(_ta); _ta = nullptr; }
    }
private:
    static void ready_cb(lv_event_t* e)
    { auto* s = static_cast<ChatWindow*>(lv_event_get_user_data(e)); if (s) s->_pendingSend = true; }

    lv_obj_t* addBubble(const std::string& text, bool user)
    {
        auto b = std::make_unique<Container>(_conv->get());
        b->setSize(LV_PCT(92), LV_SIZE_CONTENT); b->setRadius(10);
        b->setBgColor(lv_color_hex(user ? 0x1f4a3a : 0x12241c)); b->setBorderWidth(0);
        lv_obj_set_style_pad_all(b->get(), 10, 0);
        if (user) lv_obj_set_style_align(b->get(), LV_ALIGN_TOP_RIGHT, 0);
        auto l = std::make_unique<Label>(b->get());
        l->setTextFont(&lv_font_montserrat_24);
        l->setTextColor(lv_color_hex(user ? COL_VALUE : COL_LABEL));
        lv_obj_set_width(l->get(), LV_PCT(100));
        lv_label_set_long_mode(l->get(), LV_LABEL_LONG_WRAP);
        l->setText(text);
        lv_obj_t* ret = l->get();
        _bubbleLabels.push_back(std::move(l));
        _bubbles.push_back(std::move(b));
        return ret;
    }
    std::unique_ptr<Container> _conv, _send;
    std::unique_ptr<Label> _sendLab;
    std::vector<std::unique_ptr<Container>> _bubbles;
    std::vector<std::unique_ptr<Label>> _bubbleLabels;
    lv_obj_t* _ta = nullptr;
    lv_obj_t* _pendBubble = nullptr;
    lv_group_t* _grp = nullptr;
    bool _busy = false, _pendingSend = false;
};

/* --------------------------- ventana Music (Winamp) ----------------------- */
static bool is_mp3(const std::string& n)
{
    if (n.size() < 4) return false;
    std::string s = n.substr(n.size() - 4);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s == ".mp3";
}

class MusicWindow : public ui::Window {
public:
    MusicWindow()
    {
        config.title = "MUSIC"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 1040, 600, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        // Visualizador EQ (barras) + now playing
        _eq = lv_obj_create(root);
        lv_obj_set_size(_eq, 980, 90); lv_obj_align(_eq, LV_ALIGN_TOP_MID, 0, 54);
        lv_obj_set_style_bg_color(_eq, lv_color_hex(0x0a120f), 0); lv_obj_set_style_border_width(_eq, 0, 0);
        lv_obj_set_style_radius(_eq, 8, 0); lv_obj_clear_flag(_eq, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < NBARS; i++) {
            _bars[i] = lv_obj_create(_eq);
            lv_obj_set_size(_bars[i], 16, 8);
            lv_obj_set_pos(_bars[i], 16 + i * 30, 70);
            lv_obj_set_style_bg_color(_bars[i], lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_border_width(_bars[i], 0, 0); lv_obj_set_style_radius(_bars[i], 2, 0);
        }
        _now = std::make_unique<Label>(root);
        _now->align(LV_ALIGN_TOP_MID, 0, 156); _now->setTextFont(&lv_font_montserrat_24);
        _now->setTextColor(lv_color_hex(COL_ACCENT)); _now->setText("-- stopped --");
        // Playlist
        _list = std::make_unique<Container>(root);
        _list->align(LV_ALIGN_TOP_MID, 0, 190); _list->setSize(980, 280);
        _list->setBorderWidth(0); _list->setBgColor(lv_color_hex(0x0a120f)); _list->setRadius(8);
        lv_obj_set_flex_flow(_list->get(), LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(_list->get(), 2, 0); lv_obj_set_style_pad_all(_list->get(), 6, 0);
        lv_obj_set_scroll_dir(_list->get(), LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_list->get(), LV_SCROLLBAR_MODE_AUTO);
        // Controles
        int bx = 200; const int step = 140;
        addBtn(root, LV_SYMBOL_PREV " Prev", bx, [this]{ playRel(-1); }); bx += step;
        _playLbl = addBtn(root, LV_SYMBOL_PLAY " Play", bx, [this]{ togglePlay(); }); bx += step;
        addBtn(root, LV_SYMBOL_NEXT " Next", bx, [this]{ playRel(1); }); bx += step;
        _dirty = true;
    }
    void onUpdate() override
    {
        if (_state != Opened) return;
        if (_dirty) { _dirty = false; rebuild(); }
        servicePending();   // lanza el track pendiente cuando el player queda libre
        bool playing = GetHAL()->musicIsPlaying();
        // Auto-avanzar SOLO si el track acabo solo (sin cambio/parada del usuario).
        if (_wasPlaying && !playing && _pending < 0 && !_userStopped && !_files.empty())
            playRel(1);
        _wasPlaying = playing;
        // Now playing + boton
        if (_now) _now->setText(playing ? (std::string(LV_SYMBOL_AUDIO "  ") + GetHAL()->musicNowPlaying())
                                        : "-- stopped --");
        if (_playLbl) _playLbl->setText(playing ? LV_SYMBOL_STOP " Stop" : LV_SYMBOL_PLAY " Play");
        // Visualizador
        _tick++;
        for (int i = 0; i < NBARS; i++) {
            int h = 8;
            if (playing) { static const int p[8] = {10,26,44,62,80,58,34,18}; h = p[(_tick / 2 + i) % 8]; }
            lv_obj_set_size(_bars[i], 16, h);
            lv_obj_set_pos(_bars[i], 16 + i * 30, 78 - h);
        }
    }
    void onClose() override   // NO paramos la musica: sigue sonando en segundo plano
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _rows.clear(); _rowLabels.clear(); _btns.clear(); _btnLabels.clear();
        _now.reset(); _list.reset();
        if (_eq) { lv_obj_del(_eq); _eq = nullptr; }
    }

private:
    static const int NBARS = 14;

    Label* addBtn(lv_obj_t* parent, const std::string& txt, int x, std::function<void()> cb)
    {
        auto b = std::make_unique<Container>(parent);
        b->setSize(128, 50); b->align(LV_ALIGN_BOTTOM_MID, x - 540 + 64, -16);
        b->setRadius(8); b->setBgColor(lv_color_hex(0x16241d)); b->setBorderWidth(1);
        b->setBorderColor(lv_color_hex(0x2a3a32));
        auto l = std::make_unique<Label>(b->get());
        l->align(LV_ALIGN_CENTER, 0, 0); l->setTextFont(&lv_font_montserrat_20);
        l->setTextColor(lv_color_hex(COL_ACCENT)); l->setText(txt);
        b->onClick().connect(cb);
        Label* ret = l.get();
        _btnLabels.push_back(std::move(l));
        _btns.push_back(std::move(b));
        return ret;
    }

    void rebuild()
    {
        _rows.clear(); _rowLabels.clear();
        _files.clear();
        auto entries = GetHAL()->scanSdCard("/");
        for (auto& e : entries) if (!e.isDir && is_mp3(e.name)) _files.push_back(e.name);
        for (size_t i = 0; i < _files.size(); i++) addTrackRow(i);
        if (_files.empty() && _now) _now->setText("No .mp3 on SD root");
        rebindGroup();
    }
    void addTrackRow(size_t idx)
    {
        bool sel = ((int)idx == _cur);
        auto row = std::make_unique<Container>(_list->get());
        row->setSize(LV_PCT(100), 40); row->setRadius(6); row->setBorderWidth(0);
        row->setBgColor(lv_color_hex(sel ? 0x1f4a3a : 0x0a120f));
        auto l = std::make_unique<Label>(row->get());
        l->align(LV_ALIGN_LEFT_MID, 10, 0); l->setTextFont(&lv_font_montserrat_24);
        l->setTextColor(lv_color_hex(0x38EF7D));
        l->setText(std::string(LV_SYMBOL_AUDIO "  ") + _files[idx]);
        int i = (int)idx;
        row->onClick().connect([this, i]{ play(i); });
        _rowLabels.push_back(std::move(l));
        _rows.push_back(std::move(row));
    }

    void play(int idx)
    {
        if (idx < 0 || idx >= (int)_files.size()) return;
        _cur = idx; _pending = idx; _userStopped = false;
        GetHAL()->musicStop();   // onUpdate lanzara el pendiente cuando quede idle
        _dirty = true;           // refrescar resaltado
    }
    void playRel(int d)
    {
        if (_files.empty()) return;
        int n = (int)_files.size();
        int next = (((_cur < 0 ? 0 : _cur) + d) % n + n) % n;
        play(next);
    }
    void togglePlay()
    {
        if (GetHAL()->musicIsPlaying()) { GetHAL()->musicStop(); _pending = -1; _userStopped = true; }
        else play(_cur < 0 ? 0 : _cur);
    }

    void rebindGroup()
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _grp = lv_group_create();
        lv_group_set_wrap(_grp, true);
        for (auto& r : _rows) { lv_group_add_obj(_grp, r->get()); lv_obj_add_event_cb(r->get(), nav_cb, LV_EVENT_KEY, this); }
        for (auto& b : _btns) { lv_group_add_obj(_grp, b->get()); lv_obj_add_event_cb(b->get(), nav_cb, LV_EVENT_KEY, this); }
        if (!_btns.empty()) lv_group_focus_obj(_btns[1 < (int)_btns.size() ? 1 : 0]->get());
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);
    }
    static void nav_cb(lv_event_t* e)
    {
        auto* self = static_cast<MusicWindow*>(lv_event_get_user_data(e));
        if (!self || !self->_grp) return;
        switch (lv_event_get_key(e)) {
            case LV_KEY_UP: case LV_KEY_LEFT:    lv_group_focus_prev(self->_grp); break;
            case LV_KEY_DOWN: case LV_KEY_RIGHT: lv_group_focus_next(self->_grp); break;
            default: break;
        }
    }

    // Lanza el track pendiente cuando el reproductor queda libre (lo llama onUpdate base de Window via update()).
    void servicePending()
    {
        if (_pending >= 0 && !GetHAL()->musicIsPlaying()) {
            int idx = _pending; _pending = -1;
            if (idx < (int)_files.size()) GetHAL()->musicPlayFile(std::string("/sd/") + _files[idx]);
        }
    }

    bool _dirty = false, _wasPlaying = false, _userStopped = false;
    int  _cur = -1, _pending = -1, _tick = 0;
    std::vector<std::string> _files;
    std::unique_ptr<Label> _now;
    std::unique_ptr<Container> _list;
    std::vector<std::unique_ptr<Container>> _rows;
    std::vector<std::unique_ptr<Label>> _rowLabels;
    std::vector<std::unique_ptr<Container>> _btns;
    std::vector<std::unique_ptr<Label>> _btnLabels;
    Label* _playLbl = nullptr;
    lv_obj_t* _eq = nullptr;
    lv_obj_t* _bars[NBARS] = {nullptr};
    lv_group_t* _grp = nullptr;
};

/* --------------------------- ventana Power (V/A) -------------------------- */
class PowerInfoWindow : public ui::Window {
public:
    PowerInfoWindow()
    {
        config.title = "POWER MONITOR"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 760, 440, 255};
    }
    void onOpen() override
    {
        _v = std::make_unique<Label>(_window->get());
        _v->align(LV_ALIGN_CENTER, 0, -60); _v->setTextFont(&lv_font_montserrat_44);
        _v->setTextColor(lv_color_hex(COL_ACCENT)); _v->setText("-- V");
        _a = std::make_unique<Label>(_window->get());
        _a->align(LV_ALIGN_CENTER, 0, 0); _a->setTextFont(&lv_font_montserrat_44);
        _a->setTextColor(lv_color_hex(COL_VALUE)); _a->setText("-- A");
        _b = std::make_unique<Label>(_window->get());
        _b->align(LV_ALIGN_CENTER, 0, 64); _b->setTextFont(&lv_font_montserrat_28);
        _b->setTextColor(lv_color_hex(COL_LABEL)); _b->setText("battery -- %");
    }
    void onUpdate() override
    {
        if (_state != Opened) return;
        GetHAL()->updatePowerMonitorData();
        float v = GetHAL()->powerMonitorData.busVoltage;
        float a = GetHAL()->powerMonitorData.shuntCurrent;
        int pct = (int)((v - 6.0f) / 2.4f * 100.0f);
        if (pct < 0) { pct = 0; }
        if (pct > 100) { pct = 100; }
        char buf[24];
        snprintf(buf, sizeof(buf), "%.2f V", v); _v->setText(buf);
        snprintf(buf, sizeof(buf), "%.3f A", a); _a->setText(buf);
        snprintf(buf, sizeof(buf), LV_SYMBOL_BATTERY_FULL "  %d %%", pct); _b->setText(buf);
    }
    void onClose() override { _v.reset(); _a.reset(); _b.reset(); }
private:
    std::unique_ptr<Label> _v, _a, _b;
};

/* ----------------------------- ventana Tools ------------------------------ */
class ToolsWindow : public ui::Window {
public:
    ToolsWindow()
    {
        config.title = "TOOLS"; config.bgColor = COL_CARD; config.borderColor = COL_BORDER;
        config.titleColor = COL_TITLE; config.closeBtn = true;
        config.kfClosed = {0, 0, 200, 140, 0}; config.kfOpened = {0, 0, 820, 520, 255};
    }
    void onOpen() override
    {
        lv_obj_t* root = _window->get();
        struct Item { const char* icon; const char* name; int id; uint32_t col; };
        static const Item items[] = {
            {FA_IMU,       "IMU / MOTION", G_IMU,       0x38ef7d},
            {FA_GPIO,      "GPIO / IO",    G_GPIO,      0xfdbe1a},
            {FA_I2C,       "I2C SCAN",     G_I2C,       0x43d2ff},
            {FA_UART,      "UART MONITOR", G_UART,      0xc0c8c4},
            {FA_POWERINFO, "POWER (V/A)",  G_POWERINFO, 0xff8a80},
            {FA_CAMERA,    "SCREEN REC",   TOOL_SCREENREC, 0xff5f57},
        };
        _grp = lv_group_create();
        lv_group_set_wrap(_grp, true);
        for (int i = 0; i < 6; i++) {
            Item it = items[i];
            auto card = std::make_unique<Container>(root);
            card->setSize(360, 78); card->setRadius(10);
            card->setBgColor(lv_color_hex(0x12241c)); card->setBorderWidth(2);
            card->setBorderColor(lv_color_hex(COL_BORDER));
            card->align(LV_ALIGN_TOP_LEFT, 30 + (i % 2) * 380, 60 + (i / 2) * 92);
            auto ic = std::make_unique<Label>(card->get());
            ic->align(LV_ALIGN_LEFT_MID, 18, 0); ic->setTextFont(&fa_icons);
            ic->setTextColor(lv_color_hex(it.col)); ic->setText(it.icon);
            lv_obj_clear_flag(ic->get(), LV_OBJ_FLAG_CLICKABLE);
            auto nm = std::make_unique<Label>(card->get());
            nm->align(LV_ALIGN_LEFT_MID, 64, 0); nm->setTextFont(&lv_font_montserrat_24);
            nm->setTextColor(lv_color_hex(COL_LABEL)); nm->setText(it.name);
            lv_obj_clear_flag(nm->get(), LV_OBJ_FLAG_CLICKABLE);
            int id = it.id;
            card->onClick().connect([id] { if (s_view) s_view->request_tool(id); });
            lv_group_add_obj(_grp, card->get());
            lv_obj_add_event_cb(card->get(), nav_cb, LV_EVENT_KEY, this);
            _icons.push_back(std::move(ic)); _names.push_back(std::move(nm));
            _cards.push_back(std::move(card));
        }
        if (!_cards.empty()) lv_group_focus_obj(_cards[0]->get());
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _grp);
    }
    void onClose() override
    {
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_grp) { lv_group_del(_grp); _grp = nullptr; }
        _icons.clear(); _names.clear(); _cards.clear();
    }
private:
    static void nav_cb(lv_event_t* e)
    {
        auto* self = static_cast<ToolsWindow*>(lv_event_get_user_data(e));
        if (!self || !self->_grp) return;
        switch (lv_event_get_key(e)) {
            case LV_KEY_UP: case LV_KEY_LEFT:    lv_group_focus_prev(self->_grp); break;
            case LV_KEY_DOWN: case LV_KEY_RIGHT: lv_group_focus_next(self->_grp); break;
            default: break;
        }
    }
    std::vector<std::unique_ptr<Container>> _cards;
    std::vector<std::unique_ptr<Label>> _icons, _names;
    lv_group_t* _grp = nullptr;
};

/* ------------------------------- tarjetas --------------------------------- */
static void tile_click_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_view) s_view->open_tile(idx);
}

#define COL_FOCUS 0x43d2ff   // cian: halo de foco por teclado (distinto del verde)

static void style_focus_halo(lv_obj_t* o)
{
    // Foco SUTIL: matar el outline del tema (la "neblina") y solo aclarar el fondo.
    lv_obj_set_style_outline_width(o, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(o, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x16382b), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_FOCUS), LV_STATE_FOCUSED);
}

static lv_obj_t* make_grid_tile(lv_obj_t* parent, int gi)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 266, 126);  // ~20% mas pequenas
    // Fondo con degradado vertical (profundidad)
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    // Sombra suave -> sensacion de profundidad / elevacion
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_y(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16382b), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    style_focus_halo(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_label_create(card);
    lv_label_set_text(icon, GRID[gi].icon);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_ACCENT), 0);  // icono en acento
    lv_obj_set_style_text_font(icon, &fa_icons, 0);                  // FontAwesome
    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, GRID[gi].name);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_top(name, 10, 0);

    lv_obj_add_event_cb(card, tile_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)gi);
    return card;
}

static lv_obj_t* make_quick_btn(lv_obj_t* parent, const char* icon, int action)
{
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_size(b, 66, 66);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16382b), LV_STATE_PRESSED);
    style_focus_halo(b);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, icon); lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_ICON), 0);
    lv_obj_set_style_text_font(l, &fa_icons, 0);  // FontAwesome
    lv_obj_add_event_cb(b, tile_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)action);
    return b;
}

static lv_obj_t* status_item(lv_obj_t* bar, const char* init)
{
    lv_obj_t* l = lv_label_create(bar);
    lv_label_set_text(l, init);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_LABEL), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    return l;
}

// Siembra el RTC con la hora de compilacion si esta sin sincronizar (año<2024).
// El NTP automatico llegara con el WiFi (Fase B).
static void seed_rtc_if_needed()
{
    struct tm t = {};
    GetHAL()->getRtcTime(&t);
    if (t.tm_year + 1900 >= 2024) return;

    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mstr[4] = {0};
    int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mstr, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    int mon = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(months + i * 3, mstr, 3) == 0) { mon = i; break; }
    }
    struct tm nt = {};
    nt.tm_year = year - 1900; nt.tm_mon = mon; nt.tm_mday = day;
    nt.tm_hour = hh; nt.tm_min = mm; nt.tm_sec = ss;
    GetHAL()->setRtcTime(nt);
    mclog::tagInfo(_tag, "RTC sembrado con hora de compilacion: {}:{}", hh, mm);
}

/* ------------------------------ LauncherView ------------------------------ */
void LauncherView::init()
{
    mclog::tagInfo(_tag, "init");
    s_view = this;
    s_time = s_batt = s_pow = s_bigclock = s_bigdate = nullptr;
    apply_theme(GetHAL()->getSettingInt("theme", 0));  // tema guardado (persistente)
    seed_rtc_if_needed();

    ui::signal_window_opened().clear();
    ui::signal_window_opened().connect([&](bool opened) { _is_stacked = opened; });

    LvglLockGuard lock;
    buildHome();
    showLock();   // si hay PIN configurado, tapa el home hasta desbloquear
}

// Repinta el home con el tema actual SIN reiniciar: limpia la pantalla y la
// reconstruye. Lo pide SettingsWindow al cambiar de tema.
void LauncherView::requestThemeReload() { _theme_dirty = true; }

void LauncherView::buildHome()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // --- Fondo tecnico: rejilla tenue + linea de escaneo lenta (sensacion de
    //     profundidad sin recargar). Va primero -> queda detras de todo.
    //     Se puede desactivar desde Settings (bg_fx=0). ---
    if (GetHAL()->getSettingInt("bg_fx", 1)) {
    _bg = lv_obj_create(scr);
    lv_obj_remove_style_all(_bg);
    lv_obj_set_size(_bg, 1280, 720);
    lv_obj_align(_bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_bg, LV_OBJ_FLAG_CLICKABLE);
    {
        // color de rejilla = mezcla muy tenue hacia el acento (apagado)
        uint32_t gcol = (COL_BG & 0xfefefe) + ((COL_BORDER & 0xfefefe) >> 1);
        const int STEP = 64;
        for (int x = STEP; x < 1280; x += STEP) {  // verticales
            lv_obj_t* ln = lv_obj_create(_bg);
            lv_obj_remove_style_all(ln);
            lv_obj_set_size(ln, 1, 720); lv_obj_set_pos(ln, x, 0);
            lv_obj_set_style_bg_color(ln, lv_color_hex(gcol), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_30, 0);
        }
        for (int y = STEP; y < 720; y += STEP) {    // horizontales
            lv_obj_t* ln = lv_obj_create(_bg);
            lv_obj_remove_style_all(ln);
            lv_obj_set_size(ln, 1280, 1); lv_obj_set_pos(ln, 0, y);
            lv_obj_set_style_bg_color(ln, lv_color_hex(gcol), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_30, 0);
        }
        // linea de escaneo (acento, tenue) que baja despacio
        _scan = lv_obj_create(_bg);
        lv_obj_remove_style_all(_scan);
        lv_obj_set_size(_scan, 1280, 2); lv_obj_set_pos(_scan, 0, 0);
        lv_obj_set_style_bg_color(_scan, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(_scan, LV_OPA_20, 0);
        _scan_y = 0;
    }
    }  // fin if(bg_fx)

    // Título
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "// LOADOUT");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

    // Banner "Keyboard connected" (oculto por defecto)
    _kb_banner = lv_label_create(scr);
    lv_label_set_text(_kb_banner, LV_SYMBOL_KEYBOARD "  Keyboard connected");
    lv_obj_set_style_text_color(_kb_banner, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(_kb_banner, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(_kb_banner, lv_color_hex(0x123026), 0);
    lv_obj_set_style_bg_opa(_kb_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_kb_banner, 10, 0);
    lv_obj_set_style_radius(_kb_banner, 8, 0);
    lv_obj_align(_kb_banner, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_flag(_kb_banner, LV_OBJ_FLAG_HIDDEN);

    // Barra de estado (derecha, a la altura del título): hora | bateria | power
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 22, 0);
    // Barra de estado minimalista: teclado + bateria. La hora va en el reloj
    // grande de abajo; el voltaje/amperios estan en Tools -> Power.
    s_kb   = status_item(bar, LV_SYMBOL_KEYBOARD);
    s_batt = status_item(bar, LV_SYMBOL_BATTERY_FULL " --%");
    // s_time / s_pow quedan a nullptr (no se muestran arriba).
    lv_obj_set_style_text_color(s_kb, lv_color_hex(0x4a5550), 0);  // gris = sin teclado

    // Botones rápidos verticales (derecha): brillo, volumen
    lv_obj_t* quick = lv_obj_create(scr);
    lv_obj_remove_style_all(quick);
    lv_obj_set_size(quick, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(quick, LV_ALIGN_TOP_RIGHT, -20, 78);
    lv_obj_set_flex_flow(quick, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(quick, 14, 0);
    _focusables.push_back(make_quick_btn(quick, FA_BRIGHT, A_BRIGHT));
    _focusables.push_back(make_quick_btn(quick, FA_VOL, A_SPEAKER));
    _focusables.push_back(make_quick_btn(quick, FA_WIFI, A_WIFI));
    _focusables.push_back(make_quick_btn(quick, FA_TOOLS, A_TOOLS));
    _focusables.push_back(make_quick_btn(quick, FA_SETTINGS, A_SETTINGS));
    _focusables.push_back(make_quick_btn(quick, FA_ABOUT, A_ABOUT));
    _focusables.push_back(make_quick_btn(quick, FA_POWER, A_POWER));

    // Rejilla de tiles CENTRADA en el area a la izquierda del sidebar.
    _grid = lv_obj_create(scr);
    lv_obj_set_size(_grid, 1150, 540);
    lv_obj_align(_grid, LV_ALIGN_TOP_LEFT, 16, 64);
    lv_obj_set_style_bg_opa(_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_grid, 0, 0);
    lv_obj_set_style_pad_all(_grid, 6, 0);
    lv_obj_set_style_pad_row(_grid, 18, 0);
    lv_obj_set_style_pad_column(_grid, 18, 0);
    lv_obj_set_scrollbar_mode(_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(_grid, LV_FLEX_FLOW_ROW_WRAP);
    // CENTER en los 3 ejes -> el bloque 3x2 queda centrado (h y v).
    lv_obj_set_flex_align(_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    std::vector<lv_obj_t*> tiles;
    for (int i = 0; i < G_COUNT; i++) tiles.push_back(make_grid_tile(_grid, i));

    // Reloj grande + fecha (equilibra el espacio inferior, look tecnico).
    s_bigclock = lv_label_create(scr);
    lv_label_set_text(s_bigclock, "00:00");
    lv_obj_set_style_text_font(s_bigclock, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(s_bigclock, lv_color_hex(COL_TITLE), 0);
    lv_obj_align(s_bigclock, LV_ALIGN_BOTTOM_LEFT, 44, -72);
    s_bigdate = lv_label_create(scr);
    lv_label_set_text(s_bigdate, "");
    lv_obj_set_style_text_font(s_bigdate, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_bigdate, lv_color_hex(0x7a8a82), 0);
    lv_obj_align(s_bigdate, LV_ALIGN_BOTTOM_LEFT, 46, -36);

    // Orden navegable: primero los 9 tiles (rejilla 3xN), luego los botones rapidos.
    _nav.clear();
    for (auto t : tiles) _nav.push_back(t);
    for (auto q : _focusables) _nav.push_back(q);

    // Grupo de navegacion por teclado. La navegacion 2D (flechas) la gestiona
    // nav_key_cb; el grupo se encarga de ENTER (pulsar) y de Tab (siguiente).
    _home_group = lv_group_create();
    for (auto o : _nav) {
        lv_group_add_obj(_home_group, o);
        lv_obj_add_event_cb(o, nav_key_cb, LV_EVENT_KEY, this);
    }
    lv_group_set_wrap(_home_group, true);
    if (!_nav.empty()) lv_group_focus_obj(_nav[0]);
    if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _home_group);
}

// --- Pantalla de BLOQUEO por PIN -------------------------------------------
// Overlay a pantalla completa (en lv_layer_top, asi siempre queda por encima
// del home y absorbe el touch). Teclado numerico tactil + teclado fisico.
// Si no hay PIN configurado (NVS "pin" vacio) no hace nada. Recuperacion: poner
// pin=clear en /sd/loadout.conf y reiniciar.
void LauncherView::showLock()
{
    std::string pin = GetHAL()->getSettingStr("pin", "");
    if (pin.size() < 4) return;                 // sin PIN valido -> no se bloquea
    _pin_entry.clear();

    _lock = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(_lock);
    lv_obj_set_size(_lock, 1280, 720);
    lv_obj_set_pos(_lock, 0, 0);
    lv_obj_set_style_bg_color(_lock, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_lock, LV_OPA_COVER, 0);
    lv_obj_add_flag(_lock, LV_OBJ_FLAG_CLICKABLE);          // absorbe clicks (no pasa al home)
    lv_obj_clear_flag(_lock, LV_OBJ_FLAG_SCROLLABLE);

    // fondo tenue tipo "scanline" estatico para que no quede plano
    lv_obj_t* title = lv_label_create(_lock);
    lv_label_set_text(title, "// LOADOUT");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t* lk = lv_label_create(_lock);
    lv_label_set_text(lk, LV_SYMBOL_KEYBOARD "  LOCKED");
    lv_obj_set_style_text_color(lk, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lk, &lv_font_montserrat_24, 0);
    lv_obj_align(lk, LV_ALIGN_TOP_MID, 0, 116);

    // display enmascarado del PIN tecleado
    _pin_disp = lv_label_create(_lock);
    lv_label_set_text(_pin_disp, "____");
    lv_obj_set_style_text_color(_pin_disp, lv_color_hex(COL_VALUE), 0);
    lv_obj_set_style_text_font(_pin_disp, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_letter_space(_pin_disp, 10, 0);
    lv_obj_align(_pin_disp, LV_ALIGN_TOP_MID, 0, 170);

    // teclado numerico 3x4: 1..9 / borrar 0 OK
    static const char* KEYS[12] = {"1","2","3","4","5","6","7","8","9", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK};
    static const char  CODE[12] = {'1','2','3','4','5','6','7','8','9','<','0','\n'};
    lv_obj_t* pad = lv_obj_create(_lock);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 360, 480);
    lv_obj_align(pad, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pad, 14, 0);
    lv_obj_set_style_pad_column(pad, 14, 0);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* okBtn = nullptr;
    for (int i = 0; i < 12; i++) {
        lv_obj_t* b = lv_obj_create(pad);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 104, 104);
        lv_obj_set_style_radius(b, 12, 0);
        bool special = (CODE[i] == '<' || CODE[i] == '\n');
        lv_obj_set_style_bg_color(b, lv_color_hex(special ? 0x16382b : COL_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(special ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(b, (void*)(intptr_t)CODE[i]);
        lv_obj_add_event_cb(b, lock_btn_cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, KEYS[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(CODE[i] == '\n' ? COL_ACCENT : COL_VALUE), 0);
        lv_obj_center(l);
        if (CODE[i] == '\n') okBtn = b;
    }

    // Teclado fisico: un solo objeto en el grupo (OK) recibe TODAS las teclas
    // (digitos -> append, backspace -> borra, enter -> click=OK=submit). Asi no
    // hay ambiguedad de foco con los digitos tecleados.
    _lock_grp = lv_group_create();
    if (okBtn) {
        lv_group_add_obj(_lock_grp, okBtn);
        lv_group_focus_obj(okBtn);
        lv_obj_add_event_cb(okBtn, lock_key_cb, LV_EVENT_KEY, this);
    }
    if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _lock_grp);
}

// Actualiza el display y procesa la tecla pulsada del PIN.
void LauncherView::pinKey(char c)
{
    if (!_lock || !_pin_disp) return;
    if (c == '<') {
        if (!_pin_entry.empty()) _pin_entry.pop_back();
    } else if (c == '\n') {
        if (_pin_entry == GetHAL()->getSettingStr("pin", "")) {
            _unlock_pending = true;             // desbloqueo diferido (fuera del evento)
            return;
        }
        _pin_entry.clear();
        lv_label_set_text(_pin_disp, "WRONG");
        lv_obj_set_style_text_color(_pin_disp, lv_color_hex(0xff6b6b), 0);
        return;
    } else if (c >= '0' && c <= '9') {
        if (_pin_entry.size() < 8) _pin_entry.push_back(c);
    } else {
        return;
    }
    // repintar: '*' por digito, '_' los huecos (minimo 4)
    int n = (int)_pin_entry.size();
    int slots = n < 4 ? 4 : n;
    std::string m(n, '*');
    m.append(slots - n, '_');
    lv_label_set_text(_pin_disp, m.c_str());
    lv_obj_set_style_text_color(_pin_disp, lv_color_hex(COL_VALUE), 0);
}

void LauncherView::lock_btn_cb(lv_event_t* e)
{
    auto* self = static_cast<LauncherView*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    if (self && b) self->pinKey((char)(intptr_t)lv_obj_get_user_data(b));
}

void LauncherView::lock_key_cb(lv_event_t* e)
{
    auto* self = static_cast<LauncherView*>(lv_event_get_user_data(e));
    if (!self) return;
    uint32_t k = lv_event_get_key(e);
    if (k >= '0' && k <= '9')        self->pinKey((char)k);
    else if (k == LV_KEY_BACKSPACE)  self->pinKey('<');
    // ENTER: lo gestiona el click del boton OK (submit). No duplicar aqui.
}

// Navegacion 2D con flechas en la rejilla home (3 columnas para los tiles).
void LauncherView::nav_key_cb(lv_event_t* e)
{
    auto* self = static_cast<LauncherView*>(lv_event_get_user_data(e));
    if (!self || self->_nav.empty()) return;
    uint32_t key = lv_event_get_key(e);

    // ESC cierra la ventana modal abierta (si la hay).
    if (key == LV_KEY_ESC) {
        if (self->_window) self->_window->close();
        return;
    }

    lv_obj_t* cur = lv_group_get_focused(self->_home_group);
    int idx = -1;
    for (size_t i = 0; i < self->_nav.size(); i++)
        if (self->_nav[i] == cur) { idx = (int)i; break; }
    if (idx < 0) return;

    const int N = (int)self->_nav.size();
    const int TILES = G_COUNT;   // nº de tiles de la rejilla
    const int COLS  = 3;
    int tgt = idx;
    switch (key) {
        case LV_KEY_LEFT:  tgt = idx - 1; break;
        case LV_KEY_RIGHT: tgt = idx + 1; break;
        case LV_KEY_UP:    tgt = (idx < TILES) ? idx - COLS : idx - 1; break;
        case LV_KEY_DOWN:  tgt = (idx < TILES) ? idx + COLS : idx + 1; break;
        default: return;
    }
    if (tgt < 0) tgt = 0;
    if (tgt >= N) tgt = N - 1;
    if (tgt != idx) lv_group_focus_obj(self->_nav[tgt]);
}

void LauncherView::open_tile(int index)
{
    if (_window) return;
    audio::play_next_tone_progression();
    if (index == G_LAUNCHER)      _window = std::make_unique<FirmwareWindow>();
    else if (index == G_IMU)      _window = std::make_unique<MotionWindow>();
    else if (index == G_I2C)      _window = std::make_unique<I2cWindow>();
    else if (index == G_FILES)    _window = std::make_unique<FileBrowserWindow>();
    else if (index == G_GPIO)     _window = std::make_unique<GpioWindow>();
    else if (index == G_MUSIC)    _window = std::make_unique<MusicWindow>();
    else if (index == G_ASK)      _window = std::make_unique<ChatWindow>();
    else if (index == G_OTA)      _window = std::make_unique<OtaWindow>();
    else if (index == G_CAMERA)   _window = std::make_unique<CameraWindow>();
    else if (index == G_UART)     _window = std::make_unique<UartWindow>();
    else if (index == A_BRIGHT)   _window = std::make_unique<ControlWindow>(true);
    else if (index == A_SPEAKER)  _window = std::make_unique<ControlWindow>(false);
    else if (index == A_POWER)    _window = std::make_unique<PowerWindow>();
    else if (index == A_WIFI)     _window = std::make_unique<WifiWindow>();
    else if (index == A_SETTINGS) _window = std::make_unique<SettingsWindow>();
    else if (index == A_ABOUT)    _window = std::make_unique<AboutWindow>();
    else if (index == A_TOOLS)    _window = std::make_unique<ToolsWindow>();
    else if (index == G_POWERINFO)_window = std::make_unique<PowerInfoWindow>();
    else {
        const char* nm = (index >= 0 && index < G_COUNT) ? GRID[index].name : "Unknown";
        _window = std::make_unique<InfoWindow>(nm, "Coming soon");
    }
    _window->init(lv_screen_active());
    _window->open();
}

// Cierra la ventana Tools y abre la sub-herramienta (apertura diferida, fuera
// del manejador del click, igual que request_edit).
void LauncherView::request_tool(int index)
{
    _pending_tool = index;
}

// Arranca/para la grabacion de pantalla + indicador REC en la capa superior
// (lv_layer_top, asi NO sale en la grabacion). Llamado desde update() (lock ya tomado).
void LauncherView::toggleScreenRec()
{
    bool now = GetHAL()->screenRecToggle();
    if (now && !_rec_dot) {
        _rec_dot = lv_obj_create(lv_layer_top());
        lv_obj_set_size(_rec_dot, 124, 44);
        lv_obj_align(_rec_dot, LV_ALIGN_TOP_RIGHT, -150, 16);
        lv_obj_set_style_bg_color(_rec_dot, lv_color_hex(0x401a1a), 0);
        lv_obj_set_style_border_color(_rec_dot, lv_color_hex(0xff5f57), 0);
        lv_obj_set_style_border_width(_rec_dot, 2, 0);
        lv_obj_set_style_radius(_rec_dot, 22, 0);
        lv_obj_clear_flag(_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* l = lv_label_create(_rec_dot);
        lv_label_set_text(l, LV_SYMBOL_VIDEO "  REC");
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_hex(0xff5f57), 0);
        lv_obj_add_event_cb(_rec_dot, [](lv_event_t*) { GetHAL()->screenRecToggle(); },
                            LV_EVENT_CLICKED, nullptr);
    }
}

void LauncherView::request_edit(const std::string& path)
{
    // Solo marcar. NO cerrar aqui: esto se llama desde un manejador de click y
    // destruir la ventana (y sus objetos LVGL) durante el procesado del evento
    // provoca un use-after-free en el indev. update() cierra de forma diferida.
    _pending_edit = path;
}

void LauncherView::update()
{
    LvglLockGuard lock;

    // Desbloqueo diferido (PIN correcto): destruir el overlay FUERA del evento
    // del boton y devolver el teclado al home.
    if (_unlock_pending) {
        _unlock_pending = false;
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_lock_grp) { lv_group_del(_lock_grp); _lock_grp = nullptr; }
        if (_lock) { lv_obj_del(_lock); _lock = nullptr; }
        _pin_disp = nullptr; _pin_entry.clear();
        if (_home_group && GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, _home_group);
    }

    // Fondo: linea de escaneo CALMADA (independiente del framerate, ~14s por barrido).
    if (_scan && !_window) {
        uint32_t now = GetHAL()->millis();
        if (now - _scan_t >= 40) {
            _scan_t = now;
            _scan_y += 2;
            if (_scan_y > 720) _scan_y = -40;
            lv_obj_set_y(_scan, _scan_y);
        }
    }

    // Aviso de teclado (conexion / desconexion) + indicador permanente.
    bool kb = GetHAL()->isKeyboardConnected();
    if (kb != _kb_prev) {
        if (_kb_banner) {
            lv_label_set_text(_kb_banner, kb ? LV_SYMBOL_KEYBOARD "  Keyboard connected"
                                             : LV_SYMBOL_KEYBOARD "  Keyboard disconnected");
            lv_obj_set_style_bg_color(_kb_banner, lv_color_hex(kb ? 0x123026 : 0x301a1a), 0);
            lv_obj_set_style_text_color(_kb_banner, lv_color_hex(kb ? COL_ACCENT : 0xff8a80), 0);
            lv_obj_clear_flag(_kb_banner, LV_OBJ_FLAG_HIDDEN);
            _kb_banner_until = GetHAL()->millis() + 2500;
        }
        if (s_kb) {
            lv_obj_set_style_text_color(s_kb, lv_color_hex(kb ? COL_ACCENT : 0x4a5550), 0);
        }
    }
    _kb_prev = kb;
    if (_kb_banner_until && GetHAL()->millis() > _kb_banner_until && _kb_banner) {
        lv_obj_add_flag(_kb_banner, LV_OBJ_FLAG_HIDDEN);
        _kb_banner_until = 0;
    }
    // Indicador de teclado: solo el icono (color segun conexion, gestionado arriba).
    if (s_kb) lv_label_set_text(s_kb, LV_SYMBOL_KEYBOARD);

    static int tick = 0;
    if (++tick % 15 == 0) {
        char buf[40];
        // Hora RTC
        struct tm t = {};
        GetHAL()->getRtcTime(&t);
        if (s_time) { snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min); lv_label_set_text(s_time, buf); }
        if (s_bigclock) { snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min); lv_label_set_text(s_bigclock, buf); }
        if (s_bigdate) {
            static const char* wd[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
            // Calcular el dia de la semana desde la fecha (Sakamoto), NO confiar
            // en tm_wday del RTC: los formularios no lo rellenan y salia SUN fijo.
            static const int sk[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
            int Y = t.tm_year + 1900, Mo = t.tm_mon + 1, D = t.tm_mday;
            int yy = (Mo < 3) ? Y - 1 : Y;
            int w = (yy + yy/4 - yy/100 + yy/400 + sk[Mo - 1] + D) % 7;
            if (w < 0 || w > 6) w = 0;
            snprintf(buf, sizeof(buf), "%s  %04d-%02d-%02d", wd[w], Y, Mo, D);
            lv_label_set_text(s_bigdate, buf);
        }
        // Power + bateria
        GetHAL()->updatePowerMonitorData();
        float v = GetHAL()->powerMonitorData.busVoltage;
        float a = GetHAL()->powerMonitorData.shuntCurrent;
        if (s_pow) { snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %.2fV %.2fA", v, a); lv_label_set_text(s_pow, buf); }
        if (s_batt) {
            int pct = (int)((v - 6.0f) / 2.4f * 100.0f);
            if (pct < 0) { pct = 0; }
            if (pct > 100) { pct = 100; }
            snprintf(buf, sizeof(buf), LV_SYMBOL_BATTERY_FULL " %d%%", pct);
            lv_label_set_text(s_batt, buf);
        }
    }

    if (_window) {
        _window->update();
        // Cierre diferido para abrir el editor o una sub-herramienta (desde un click).
        if ((!_pending_edit.empty() || _pending_tool >= 0) &&
            _window->getState() == ui::Window::State_t::Opened)
            _window->close();
        if (_window->getState() == ui::Window::State_t::Closed) _window.reset();
    }

    // Sub-herramienta de Tools cuando se haya cerrado la ventana Tools.
    if (!_window && _pending_tool != -1) {
        int t = _pending_tool; _pending_tool = -1;
        if (t == TOOL_SCREENREC) toggleScreenRec();  // accion especial (no abre ventana)
        else { open_tile(t); }
        return;
    }
    // Quitar el indicador REC cuando la grabacion ha parado del todo.
    if (_rec_dot && !GetHAL()->screenRecActive()) {
        lv_obj_del(_rec_dot); _rec_dot = nullptr;
    }

    // Repintado de tema en vivo (sin reiniciar): solo con la pantalla despejada.
    if (!_window && _pending_edit.empty() && _pending_tool < 0 && _theme_dirty) {
        _theme_dirty = false;
        // Teardown del home actual.
        if (GetHAL()->lvKeyboard) lv_indev_set_group(GetHAL()->lvKeyboard, nullptr);
        if (_home_group) { lv_group_del(_home_group); _home_group = nullptr; }
        _focusables.clear(); _nav.clear();
        _kb_banner = nullptr; _grid = nullptr; _bg = nullptr; _scan = nullptr;
        s_kb = s_time = s_batt = s_pow = s_bigclock = s_bigdate = nullptr;
        lv_obj_clean(lv_screen_active());  // borra todos los hijos (chrome + grid)
        buildHome();                       // reconstruye con el tema actual
        return;
    }

    // Abrir el editor de texto cuando se haya cerrado la ventana anterior.
    if (!_window && !_pending_edit.empty()) {
        _window = std::make_unique<TextEditorWindow>(_pending_edit);
        _pending_edit.clear();
        _window->init(lv_screen_active());
        _window->open();
    }

    // Sin ventana abierta (y sin bloqueo): el teclado controla la rejilla.
    if (!_window && !_lock && _home_group && GetHAL()->lvKeyboard &&
        lv_indev_get_group(GetHAL()->lvKeyboard) != _home_group) {
        lv_indev_set_group(GetHAL()->lvKeyboard, _home_group);
    }
}
