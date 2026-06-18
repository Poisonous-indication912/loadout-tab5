/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <app.h>
#include <hal/hal.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_ota_ops.h>

extern "C" void app_main(void)
{
    // LOADOUT corre desde una ranura OTA (ota_0/ota_1). Nos marcamos como imagen
    // VALIDA para cancelar el rollback: asi el launcher persiste tras un auto-update
    // y el bootloader vuelve a esta ranura cuando un firmware externo no se valida.
    esp_ota_mark_app_valid_cancel_rollback();

    // 应用层初始化回调
    app::InitCallback_t callback;

    callback.onHalInjection = []() {
        // 注入桌面平台的硬件抽象
        hal::Inject(std::make_unique<HalEsp32>());
    };

    // 应用层启动
    app::Init(callback);
    while (!app::IsDone()) {
        app::Update();
        vTaskDelay(1);
    }
    app::Destroy();
}
