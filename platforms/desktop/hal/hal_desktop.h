/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <map>

class HalDesktop : public hal::HalBase {
public:
    std::string type() override
    {
        return "Desktop";
    }

    void init() override;

    // Ajustes en memoria (el sim no tiene NVS): persisten dentro de la sesion,
    // asi temas / bg_fx / PIN se comportan como en el device. "pin" se siembra
    // desde la env var LOADOUT_PIN para poder probar el bloqueo.
    int getSettingInt(const char* key, int def) override
    {
        auto it = _si.find(key);
        return it == _si.end() ? def : it->second;
    }
    void setSettingInt(const char* key, int value) override { _si[key] = value; }
    std::string getSettingStr(const char* key, const std::string& def) override
    {
        auto it = _ss.find(key);
        return it == _ss.end() ? def : it->second;
    }
    void setSettingStr(const char* key, const std::string& value) override { _ss[key] = value; }

    void delay(uint32_t ms) override;
    uint32_t millis() override;
    int getCpuTemp() override;

    void setDisplayBrightness(uint8_t brightness) override;
    uint8_t getDisplayBrightness() override;

    void lvglLock() override;
    void lvglUnlock() override;

    void setSpeakerVolume(uint8_t volume) override;
    uint8_t getSpeakerVolume() override;
    void audioPlay(std::vector<int16_t>& data, bool async = true) override;
    void audioRecord(std::vector<int16_t>& data, uint16_t durationMs, float gain = 80.0f) override;
    void startDualMicRecordTest() override;
    MicTestState_t getDualMicRecordTestState() override;
    void startHeadphoneMicRecordTest() override;
    MicTestState_t getHeadphoneMicRecordTestState() override;
    void startPlayMusicTest() override;
    MusicPlayState_t getMusicPlayTestState() override;
    void stopPlayMusicTest() override;

    void updatePowerMonitorData() override;
    void setChargeQcEnable(bool enable) override;
    bool getChargeQcEnable() override;
    void setChargeEnable(bool enable) override;
    bool getChargeEnable() override;
    void setUsb5vEnable(bool enable) override;
    bool getUsb5vEnable() override;
    void setExt5vEnable(bool enable) override;
    bool getExt5vEnable() override;

    void updateImuData() override;

    void setExtAntennaEnable(bool enable) override;
    bool getExtAntennaEnable() override;

    bool isSdCardMounted() override;
    std::vector<FileEntry_t> scanSdCard(const std::string& dirPath) override;

    bool usbCDetect() override;
    bool usbADetect() override;
    bool headPhoneDetect() override;
    std::vector<uint8_t> i2cScan(bool isInternal) override;

    void uartMonitorSend(std::string msg, bool newLine = true) override;

private:
    uint8_t _current_lcd_brightness = 100;
    uint8_t _current_speaker_volume = 20;
    bool _charge_qc_enable          = false;
    bool _charge_enable             = true;
    bool _ext_5v_enable             = true;
    bool _usba_5v_enable            = true;
    bool _ext_antenna_enable        = false;
    std::map<std::string, int> _si;
    std::map<std::string, std::string> _ss;

    void lvgl_init();
};
