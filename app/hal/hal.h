/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <lvgl.h>
#include <mutex>
#include <vector>

/**
 * @brief Hardware abstraction layer
 *
 */
namespace hal {

/**
 * @brief
 *
 */
class HalBase {
public:
    virtual ~HalBase() = default;

    /**
     * @brief
     *
     * @return std::string
     */
    virtual std::string type()
    {
        return "Base";
    }

    /**
     * @brief
     *
     */
    virtual void init()
    {
    }

    /* --------------------------------- System --------------------------------- */
    virtual void delay(uint32_t ms)
    {
    }
    virtual uint32_t millis()
    {
        return 0;
    }
    virtual int getCpuTemp()
    {
        return 0.0f;
    }

    /* --------------------------------- Display -------------------------------- */
    virtual int getDisplayWidth()
    {
        return 1280;
    }
    virtual int getDisplayHeight()
    {
        return 720;
    }
    virtual void setDisplayBrightness(uint8_t brightness)
    {
    }
    virtual uint8_t getDisplayBrightness()
    {
        return 0;
    }
    virtual std::string getDisplayPanelIc()
    {
        return "ILI9881C";
        // return "ST7123";
    }

    /* ---------------------------------- Lvgl ---------------------------------- */
    lv_indev_t* lvTouchpad = nullptr;
    lv_indev_t* lvKeyboard = nullptr;  // teclado USB (lo crea el HAL del device)
    // Debug del teclado: lo rellena la tarea del keypad (visible en la UI).
    volatile int dbgLastKey  = 0;
    volatile int dbgKeyCount = 0;
    volatile int dbgKbAddr   = 0;  // direccion I2C del teclado detectado (0=ninguno)
    volatile int dbgKbConsumed = 0;  // teclas que LVGL ha sacado de la cola (indev read)
    virtual void lvglLock()
    {
    }
    virtual void lvglUnlock()
    {
    }

    /* ---------------------------------- Power --------------------------------- */
    struct PMData_t {
        float busVoltage   = 0.0f;
        float busPower     = 0.0f;
        float shuntVoltage = 0.0f;
        float shuntCurrent = 0.0f;
    };
    PMData_t powerMonitorData;
    virtual void updatePowerMonitorData()
    {
    }
    virtual void setChargeQcEnable(bool enable)
    {
    }
    virtual bool getChargeQcEnable()
    {
        return false;
    }
    virtual void setChargeEnable(bool enable)
    {
    }
    virtual bool getChargeEnable()
    {
        return false;
    }
    virtual void setUsb5vEnable(bool enable)
    {
    }
    virtual bool getUsb5vEnable()
    {
        return false;
    }
    virtual void setExt5vEnable(bool enable)
    {
    }
    virtual bool getExt5vEnable()
    {
        return false;
    }
    virtual void powerOff()
    {
    }
    virtual void sleepAndTouchWakeup()
    {
    }
    virtual void sleepAndShakeWakeup()
    {
    }
    virtual void sleepAndRtcWakeup()
    {
    }

    /* ----------------------------------- IMU ---------------------------------- */
    struct IMUData_t {
        float accelX = 0.0f;
        float accelY = 0.0f;
        float accelZ = 0.0f;
        float gyroX  = 0.0f;
        float gyroY  = 0.0f;
        float gyroZ  = 0.0f;
    };
    IMUData_t imuData;
    virtual void updateImuData()
    {
    }
    virtual void clearImuIrq()
    {
    }

    /* ----------------------------------- RTC ---------------------------------- */
    virtual void getRtcTime(tm* time)
    {
    }
    virtual void setRtcTime(tm time)
    {
    }
    virtual void clearRtcIrq()
    {
    }

    /* --------------------------------- Camera --------------------------------- */
    virtual void startCameraCapture(lv_obj_t* imgCanvas)
    {
    }
    virtual void stopCameraCapture()
    {
    }
    virtual bool isCameraCapturing()
    {
        return false;
    }

    /* ---------------------------------- USB-A --------------------------------- */
    struct HidMouseData_t {
        std::mutex mutex;
        int x         = 0;
        int y         = 0;
        bool btnLeft  = false;
        bool btnRight = false;
    };
    HidMouseData_t hidMouseData;

    /* ---------------------------------- Audio --------------------------------- */
    virtual void setSpeakerVolume(uint8_t volume)
    {
    }
    virtual uint8_t getSpeakerVolume()
    {
        return 0;
    }
    // [MIC-L, AEC, MIC-R, MIC-HP]
    virtual void audioRecord(std::vector<int16_t>& data, uint16_t durationMs, float gain = 80.0f)
    {
    }
    virtual void audioPlay(std::vector<int16_t>& data, bool async = true)
    {
    }

    // Mic record test
    enum MicTestState_t {
        MIC_TEST_IDLE,
        MIC_TEST_RECORDING,
        MIC_TEST_PLAYING,
    };
    virtual void startDualMicRecordTest()
    {
    }
    virtual MicTestState_t getDualMicRecordTestState()
    {
        return MIC_TEST_IDLE;
    }
    virtual void startHeadphoneMicRecordTest()
    {
    }
    virtual MicTestState_t getHeadphoneMicRecordTestState()
    {
        return MIC_TEST_IDLE;
    }

    // Play music test
    enum MusicPlayState_t {
        MUSIC_PLAY_IDLE,
        MUSIC_PLAY_PLAYING,
    };
    // Reproductor de ficheros .mp3 de la SD (device-only).
    virtual bool musicPlayFile(const std::string& path)
    {
        return false;
    }
    virtual void musicStop()
    {
    }
    virtual bool musicIsPlaying()
    {
        return false;
    }
    virtual std::string musicNowPlaying()
    {
        return "";
    }
    virtual void startPlayMusicTest()
    {
    }
    virtual MusicPlayState_t getMusicPlayTestState()
    {
        return MUSIC_PLAY_IDLE;
    }
    virtual void stopPlayMusicTest()
    {
    }

    // Grabador de pantalla (device-only): JPEG por HW -> .jpg en /sd/rec/.
    virtual bool screenRecToggle()  // arranca/para; devuelve true si quedo grabando
    {
        return false;
    }
    virtual bool screenRecActive()
    {
        return false;
    }
    virtual int screenRecFrames()
    {
        return 0;
    }

    // Sfx
    virtual void playDecodeSfx()   // bleeps de "decoding" para la intro
    {
    }
    virtual void playStartupSfx()
    {
    }
    virtual void playShutdownSfx()
    {
    }

    /* --------------------------------- Network -------------------------------- */
    virtual void setExtAntennaEnable(bool enable)
    {
    }
    virtual bool getExtAntennaEnable()
    {
        return false;
    }
    virtual void startWifiAp()
    {
    }
    // Estado de la conexion STA (a tu WiFi de casa). Device-only.
    virtual bool wifiStaConnected()
    {
        return false;
    }
    virtual std::string wifiStaInfo()
    {
        return "n/a";
    }
    // OTA self-update (descarga de GitHub manifest). Device-only.
    virtual void otaStartCheck()
    {
    }
    virtual void otaStartUpdate()
    {
    }
    virtual int otaState()  // 0 idle,1 checking,2 checked,3 updating,4 error
    {
        return 0;
    }
    virtual std::string otaLatestVersion()
    {
        return "";
    }
    virtual std::string otaMessage()
    {
        return "";
    }
    // Chatbot IA (API estilo OpenAI por HTTPS). Device-only.
    virtual void chatSend(const std::string& prompt)
    {
    }
    virtual int chatState()  // 0 idle,1 sending,2 done,3 error
    {
        return 0;
    }
    virtual std::string chatReply()
    {
        return "";
    }
    virtual void chatReset()
    {
    }

    /* --------------------------------- SD Card -------------------------------- */
    struct FileEntry_t {
        std::string name;
        bool isDir;
    };
    virtual bool isSdCardMounted()
    {
        return false;
    }
    virtual std::vector<FileEntry_t> scanSdCard(const std::string& dirPath)
    {
        return {};
    }
    // Arranca un firmware .bin de la SD (lo flashea a ota_0 y reinicia).
    // Solo implementado en el HAL del device; en escritorio es un stub.
    virtual bool bootFirmware(const std::string& path)
    {
        return false;
    }
    // Flasheo asincrono con progreso (device-only). flashProgress(): -1 idle,
    // 0..100 flasheando, -2 error. En exito el device reinicia.
    virtual void bootFirmwareAsync(const std::string& path)
    {
    }
    virtual int flashProgress()
    {
        return -1;
    }
    // Lectura/escritura de ficheros de texto de la SD (para el editor).
    virtual std::string readTextFile(const std::string& path)
    {
        return "";
    }
    virtual bool writeTextFile(const std::string& path, const std::string& content)
    {
        return false;
    }
    // Operaciones de fichero (rutas completas tipo "/sd/..."). Device-only; stub en escritorio.
    virtual bool deletePath(const std::string& path)  // borra fichero o carpeta (recursivo)
    {
        return false;
    }
    virtual bool makeDir(const std::string& path)
    {
        return false;
    }
    virtual bool createFile(const std::string& path)
    {
        return false;
    }
    virtual bool copyFile(const std::string& src, const std::string& dst)
    {
        return false;
    }
    virtual bool movePath(const std::string& src, const std::string& dst)  // mover/renombrar
    {
        return false;
    }
    // Ajustes persistentes (NVS en el device; stub en escritorio).
    virtual int getSettingInt(const char* key, int def)
    {
        return def;
    }
    virtual void setSettingInt(const char* key, int value)
    {
    }
    virtual std::string getSettingStr(const char* key, const std::string& def)
    {
        return def;
    }
    virtual void setSettingStr(const char* key, const std::string& value)
    {
    }

    /* -------------------------------- Interface ------------------------------- */
    virtual bool usbCDetect()
    {
        return false;
    }
    virtual bool usbADetect()
    {
        return false;
    }
    // ¿Hay un teclado USB conectado? (para toast + habilitar editor)
    virtual bool isKeyboardConnected()
    {
        return false;
    }
    virtual bool headPhoneDetect()
    {
        return false;
    }
    virtual std::vector<uint8_t> i2cScan(bool isInternal)
    {
        return {};
    }
    virtual void initPortAI2c()
    {
    }
    virtual void deinitPortAI2c()
    {
    }

    virtual void gpioInitOutput(uint8_t pin)
    {
    }
    virtual void gpioSetLevel(uint8_t pin, bool level)
    {
    }
    virtual void gpioReset(uint8_t pin)
    {
    }

    /* ------------------------------ UART monitor ------------------------------ */
    struct UartMonitorData_t {
        std::mutex mutex;
        std::queue<uint8_t> rxQueue;
        std::queue<uint8_t> txQueue;
    };
    UartMonitorData_t uartMonitorData;
    virtual void uartMonitorSend(std::string msg, bool newLine = true)
    {
        std::lock_guard<std::mutex> lock(uartMonitorData.mutex);
        for (auto c : msg) {
            uartMonitorData.txQueue.push(c);
        }
        if (newLine) {
            uartMonitorData.txQueue.push('\n');
        }
    }
};

/**
 * @brief Get the HAL instance
 *
 * @return HalBase&
 */
HalBase* Get();

/**
 * @brief Inject the HAL, which will call init() to initialize the HAL
 *
 * @param hal
 */
void Inject(std::unique_ptr<HalBase> hal);

/**
 * @brief Destroy the HAL instance
 *
 */
void Destroy();

/**
 * @brief Check if the HAL instance exists
 *
 * @return true
 * @return false
 */
bool Check();

}  // namespace hal

/**
 * @brief Get the HAL instance
 *
 * @return hal::HalBase&
 */
inline hal::HalBase* GetHAL()
{
    return hal::Get();
}

/**
 * @brief
 *
 */
class LvglLockGuard {
public:
    LvglLockGuard()
    {
        GetHAL()->lvglLock();
    }
    ~LvglLockGuard()
    {
        GetHAL()->lvglUnlock();
    }
};
