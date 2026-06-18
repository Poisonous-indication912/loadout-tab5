/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 *
 * Intro LOADOUT: efecto decryption / text-scramble (estilo hacker). Los caracteres
 * de "// LOADOUT" se van "desencriptando" de izquierda a derecha, luego una barra
 * de carga, con un SFX de "decoding". Reemplaza la animación + logo + SFX del TAB5.
 */
#pragma once
#include <mooncake.h>
#include <memory>
#include <lvgl.h>
#include <stdint.h>

class AppStartupAnim : public mooncake::AppAbility {
public:
    AppStartupAnim();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum St { S_DECRYPT = 0, S_HOLD };
    St _state = S_DECRYPT;

    lv_obj_t* _load   = nullptr;   // texto grande (scramble)
    lv_obj_t* _status = nullptr;   // linea de estado (DECRYPTING / LOADING + barra)

    uint32_t _t0       = 0;   // inicio del estado actual
    uint32_t _tstep    = 0;   // último refresco del scramble
    unsigned int _rng  = 0x2545F491u;
    bool _wifi_started = false;
    bool _sfx_started  = false;
};
