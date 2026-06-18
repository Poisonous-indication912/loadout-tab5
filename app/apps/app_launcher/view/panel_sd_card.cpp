/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "lvgl_cpp/label.h"
#include "view.h"
#include <lvgl.h>
#include <hal/hal.h>
#include <memory>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/ui/window.h>
#include <apps/utils/ui/toast.h>
#include <src/widgets/label/lv_label.h>
#include <string>
#include <cctype>

using namespace launcher_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string _tag = "panel-sd";

static const ui::Window::KeyFrame_t _kf_sd_card_scan_close = {-46, 300, 75, 75, 0};
static const ui::Window::KeyFrame_t _kf_sd_card_scan_open  = {-40, 43, 566, 411, 255};

// ¿El fichero acaba en .bin? (firmware arrancable)
static bool is_bin(const std::string& n)
{
    if (n.size() < 4) {
        return false;
    }
    std::string e = n.substr(n.size() - 4);
    for (auto& c : e) {
        c = (char)tolower((unsigned char)c);
    }
    return e == ".bin";
}

class SdCardScanWindow : public ui::Window {
public:
    SdCardScanWindow()
    {
        config.title    = "SD-Card File Scan";
        config.kfClosed = _kf_sd_card_scan_close;
        config.kfOpened = _kf_sd_card_scan_open;
    }

    void onOpen() override
    {
        _window->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);

        _panel_file_entries = std::make_unique<Container>(_window->get());
        _panel_file_entries->align(LV_ALIGN_CENTER, 0, 18);
        _panel_file_entries->setSize(535, 345);
        _panel_file_entries->setBorderWidth(0);
        _panel_file_entries->setBgColor(lv_color_hex(0x393939));
        _panel_file_entries->setPadding(12, 12, 24, 24);

        _label_msg = std::make_unique<Label>(_window->get());
        _label_msg->align(LV_ALIGN_CENTER, 0, -24);
        _label_msg->setTextFont(&lv_font_montserrat_24);
        if (GetHAL()->isSdCardMounted()) {
            _label_msg->setText("Scanning SD Card ...");
        } else {
            _label_msg->setTextColor(lv_color_hex(0xFD4444));
            _label_msg->setText("SD Card not mounted.\n\nPlease insert SD Card and reboot.");
        }
    }

    // Arranca el firmware via HAL (device: flashea a ota_0 y reinicia;
    // escritorio: stub). Si retorna, es que ha fallado.
    void boot_fw(const std::string& name)
    {
        GetHAL()->bootFirmware(std::string("/sd/") + name);
        if (_label_hint) {
            _label_hint->setTextColor(lv_color_hex(0xFD4444));
            _label_hint->setText("Error al arrancar el firmware");
        }
    }

    void onUpdate() override
    {
        if (_state != Opened) {
            return;
        }

        if (GetHAL()->isSdCardMounted() && !_is_scanned) {
            _is_scanned       = true;
            auto file_entries = GetHAL()->scanSdCard("/");
            if (file_entries.empty()) {
                _label_msg->setText("No files found on SD Card.");
                return;
            }

            _label_file_entries.clear();
            _bin_buttons.clear();

            for (size_t i = 0; i < file_entries.size(); i++) {
                bool        bin   = !file_entries[i].isDir && is_bin(file_entries[i].name);
                lv_color_t  color = file_entries[i].isDir ? lv_color_hex(0xFDBE1A)
                                    : bin                 ? lv_color_hex(0x38EF7D)   // verde = arrancable
                                                          : lv_color_hex(0x43D2FF);

                // Icono.
                _label_file_entries.push_back(std::make_unique<Label>(_panel_file_entries->get()));
                _label_file_entries.back()->align(LV_ALIGN_TOP_LEFT, 0, i * 42);
                _label_file_entries.back()->setTextFont(&lv_font_montserrat_24);
                _label_file_entries.back()->setTextColor(color);
                lv_label_set_text(_label_file_entries.back()->get(),
                                  file_entries[i].isDir ? LV_SYMBOL_DIRECTORY
                                  : bin                 ? LV_SYMBOL_PLAY
                                                        : LV_SYMBOL_FILE);

                // Nombre.
                _label_file_entries.push_back(std::make_unique<Label>(_panel_file_entries->get()));
                _label_file_entries.back()->align(LV_ALIGN_TOP_LEFT, 36, i * 42);
                _label_file_entries.back()->setTextFont(&lv_font_montserrat_24);
                _label_file_entries.back()->setTextColor(color);
                _label_file_entries.back()->setText(file_entries[i].name);

                // Si es un .bin: zona pulsable transparente sobre la fila -> arranca.
                if (bin) {
                    std::string name = file_entries[i].name;
                    _bin_buttons.push_back(std::make_unique<Container>(_panel_file_entries->get()));
                    _bin_buttons.back()->align(LV_ALIGN_TOP_LEFT, 0, i * 42);
                    _bin_buttons.back()->setSize(500, 38);
                    _bin_buttons.back()->setOpa(0);
                    _bin_buttons.back()->onClick().connect([this, name] {
                        audio::play_next_tone_progression();
                        boot_fw(name);
                    });
                }
            }

            _label_msg.reset();

            // Pista de accion.
            _label_hint = std::make_unique<Label>(_window->get());
            _label_hint->align(LV_ALIGN_BOTTOM_MID, 0, -8);
            _label_hint->setTextFont(&lv_font_montserrat_20);
            _label_hint->setTextColor(lv_color_hex(0x38EF7D));
            _label_hint->setText(LV_SYMBOL_PLAY "  Toca un firmware .bin para arrancarlo");
        }
    }

    void onClose() override
    {
        audio::play_next_tone_progression();
        _label_msg.reset();
        _label_hint.reset();
        _label_file_entries.clear();
        _bin_buttons.clear();
        _panel_file_entries.reset();
    }

private:
    std::unique_ptr<Label> _label_msg;
    std::unique_ptr<Label> _label_hint;
    std::unique_ptr<Container> _panel_file_entries;
    std::vector<std::unique_ptr<Label>> _label_file_entries;
    std::vector<std::unique_ptr<Container>> _bin_buttons;
    bool _is_scanned = false;
};

void PanelSdCard::init()
{
    _btn_sd_card_scan = std::make_unique<Container>(lv_screen_active());
    _btn_sd_card_scan->align(LV_ALIGN_CENTER, -46, 300);
    _btn_sd_card_scan->setSize(100, 100);
    _btn_sd_card_scan->setOpa(0);
    _btn_sd_card_scan->onClick().connect([&] {
        audio::play_next_tone_progression();

        // Create window
        _window = std::make_unique<SdCardScanWindow>();
        _window->init(lv_screen_active());
        _window->open();
    });
}

void PanelSdCard::update(bool isStacked)
{
    if (_window) {
        _window->update();
        if (_window->getState() == ui::Window::State_t::Closed) {
            _window.reset();
        }
    }
}
