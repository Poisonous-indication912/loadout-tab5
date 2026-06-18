/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
extern "C" {
#include "utils/rx8130/rx8130.h"
}
#include <mooncake_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bsp/m5stack_tab5.h>
#include <lv_demos.h>

extern esp_lcd_touch_handle_t _lcd_touch_handle;

static const std::string _tag = "hal";

static void lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    if (_lcd_touch_handle == NULL) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t touch_x[1];
    uint16_t touch_y[1];
    uint16_t touch_strength[1];
    uint8_t touch_cnt = 0;

    esp_lcd_touch_read_data(_lcd_touch_handle);
    bool touchpad_pressed =
        esp_lcd_touch_get_coordinates(_lcd_touch_handle, touch_x, touch_y, touch_strength, &touch_cnt, 1);
    // mclog::tagInfo(_tag, "touchpad pressed: {}", touchpad_pressed);

    if (!touchpad_pressed) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = touch_x[0];
        data->point.y = touch_y[0];
    }
}

void HalEsp32::init()
{
    mclog::tagInfo(_tag, "init");

    mclog::tagInfo(_tag, "camera init");
    bsp_cam_osc_init();

    mclog::tagInfo(_tag, "i2c init");
    bsp_i2c_init();

    mclog::tagInfo(_tag, "io expander init");
    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    bsp_io_expander_pi4ioe_init(i2c_bus_handle);

    setChargeQcEnable(true);
    delay(50);
    setChargeEnable(true);
    // setChargeEnable(false);

    mclog::tagInfo(_tag, "i2c scan");
    bsp_i2c_scan();

    mclog::tagInfo(_tag, "codec init");
    delay(200);
    bsp_codec_init();

    mclog::tagInfo(_tag, "imu init");
    imu_init();

    mclog::tagInfo(_tag, "ina226 init");
    ina226.begin(i2c_bus_handle, 0x41);
    ina226.configure(INA226_AVERAGES_16, INA226_BUS_CONV_TIME_1100US, INA226_SHUNT_CONV_TIME_1100US,
                     INA226_MODE_SHUNT_BUS_CONT);
    ina226.calibrate(0.005, 8.192);
    mclog::tagInfo(_tag, "bus voltage: {}", ina226.readBusVoltage());

    mclog::tagInfo(_tag, "rx8130 init");
    rx8130.begin(i2c_bus_handle, 0x32);
    rx8130.initBat();
    clearRtcIrq();
    update_system_time();

    mclog::tagInfo(_tag, "display init");
    bsp_reset_tp();
    bsp_display_cfg_t cfg = {.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
                             .buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES,
                             .double_buffer = true,
                             .flags         = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
                                 .buff_dma = false,
#else
                                 .buff_dma = true,
#endif
                                 .buff_spiram = true,
                                 .sw_rotate   = true,
                             }};
    lvDisp = bsp_display_start_with_config(&cfg);
    lv_display_set_rotation(lvDisp, LV_DISPLAY_ROTATION_90);
    bsp_display_backlight_on();

    // // Touchpad lvgl indev
    // mclog::tagInfo(_tag, "create lvgl touchpad indev");
    // lvTouchpad = lv_indev_create();
    // lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    // lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    // lv_indev_set_display(lvTouchpad, lvDisp);

    mclog::tagInfo(_tag, "usb host init");
    bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);

    mclog::tagInfo(_tag, "hid init");
    hid_init();

    mclog::tagInfo(_tag, "rs485 init");
    rs485_init();

    mclog::tagInfo(_tag, "set gpio output capability");
    set_gpio_output_capability();

    bsp_display_unlock();
}

static const gpio_num_t _driver_gpios[] = {
    // EXT I2C
    GPIO_NUM_0,
    GPIO_NUM_1,
    // esp-hosted esp32c6
    GPIO_NUM_8,
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_15,
    // Display
    GPIO_NUM_22,
    GPIO_NUM_23,
    // Audio
    GPIO_NUM_26,
    GPIO_NUM_27,
    GPIO_NUM_28,
    GPIO_NUM_29,
    GPIO_NUM_30,
    // SYS I2C
    GPIO_NUM_31,
    GPIO_NUM_32,
    // uSD card
    GPIO_NUM_39,
    GPIO_NUM_40,
    GPIO_NUM_41,
    GPIO_NUM_42,
    GPIO_NUM_43,
    GPIO_NUM_44,
};

void HalEsp32::set_gpio_output_capability()
{
    // gpio_set_drive_capability((gpio_num_t)48, GPIO_DRIVE_CAP_0);
    for (int i = 0; i < sizeof(_driver_gpios) / sizeof(_driver_gpios[0]); i++) {
        gpio_num_t gpio = _driver_gpios[i];
        esp_err_t ret   = gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_0);
        if (ret == ESP_OK) {
            printf("GPIO %d drive capability set to GPIO_DRIVE_CAP_0\n", gpio);
        } else {
            printf("Failed to set GPIO %d drive capability: %s\n", gpio, esp_err_to_name(ret));
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
#include <driver/temperature_sensor.h>
static temperature_sensor_handle_t _temp_sensor = nullptr;

void HalEsp32::delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t HalEsp32::millis()
{
    return esp_timer_get_time() / 1000;
}

int HalEsp32::getCpuTemp()
{
    if (_temp_sensor == nullptr) {
        temperature_sensor_config_t temp_sensor_config = {
            .range_min = 20,
            .range_max = 100,
        };
        temperature_sensor_install(&temp_sensor_config, &_temp_sensor);
        temperature_sensor_enable(_temp_sensor);
    }

    float temp = 0;
    temperature_sensor_get_celsius(_temp_sensor, &temp);

    return temp;
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
void HalEsp32::setDisplayBrightness(uint8_t brightness)
{
    _current_lcd_brightness = std::clamp((int)brightness, 0, 100);
    mclog::tagInfo("hal", "set display brightness: {}%", _current_lcd_brightness);
    bsp_display_brightness_set(_current_lcd_brightness);
}

uint8_t HalEsp32::getDisplayBrightness()
{
    return _current_lcd_brightness;
}

std::string HalEsp32::getDisplayPanelIc()
{
    return std::string(bsp_display_get_panel_ic());
}

void HalEsp32::lvglLock()
{
    lvgl_port_lock(0);
}

void HalEsp32::lvglUnlock()
{
    lvgl_port_unlock();
}

/* -------------------------------------------------------------------------- */
/*                                     RTC                                    */
/* -------------------------------------------------------------------------- */
void HalEsp32::clearRtcIrq()
{
    mclog::tagInfo(_tag, "clear rtc irq");
    rx8130.clearIrqFlags();
    rx8130.disableIrq();
}

// FALTABA: sin esto getRtcTime devolvia {} (año 1900) -> seed_rtc_if_needed
// re-sembraba la hora del build en cada arranque y el reloj mostraba 00:00.
void HalEsp32::getRtcTime(tm* time)
{
    if (time) rx8130.getTime(time);
}

// Dia de la semana (0=domingo..6=sabado) a partir de la fecha (Sakamoto).
// Necesario porque los formularios (Settings/RTC) no rellenan tm_wday y se
// quedaba en 0 (SUN) siempre.
static int dow_from_date(int year, int mon1, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (mon1 < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[mon1 - 1] + day) % 7;
}

void HalEsp32::setRtcTime(tm time)
{
    time.tm_wday = dow_from_date(time.tm_year + 1900, time.tm_mon + 1, time.tm_mday);
    mclog::tagInfo(_tag, "set rtc time to {}/{}/{} {:02d}:{:02d}:{:02d}", time.tm_year + 1900, time.tm_mon + 1,
                   time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
    rx8130.setTime(&time);
    delay(50);

    update_system_time();
}

void HalEsp32::update_system_time()
{
    mclog::tagInfo(_tag, "update system time");
    struct tm time;
    rx8130.getTime(&time);
    mclog::tagInfo(_tag, "sync to rtc time: {}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", time.tm_year + 1900,
                   time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
    struct timeval now;
    now.tv_sec  = mktime(&time);
    now.tv_usec = 0;
    settimeofday(&now, NULL);
}

/* -------------------------------------------------------------------------- */
/*                                   SD Card                                  */
/* -------------------------------------------------------------------------- */
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>
#include "bsp/m5stack_tab5.h"
#include <fw_boot.h>
#include <nvs.h>

int HalEsp32::getSettingInt(const char* key, int def)
{
    nvs_handle_t h;
    if (nvs_open("loadout", NVS_READONLY, &h) != ESP_OK) return def;
    int32_t v   = def;
    esp_err_t e = nvs_get_i32(h, key, &v);
    nvs_close(h);
    return (e == ESP_OK) ? (int)v : def;
}

void HalEsp32::setSettingInt(const char* key, int value)
{
    nvs_handle_t h;
    if (nvs_open("loadout", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, (int32_t)value);
    nvs_commit(h);
    nvs_close(h);
}

std::string HalEsp32::getSettingStr(const char* key, const std::string& def)
{
    nvs_handle_t h;
    if (nvs_open("loadout", NVS_READONLY, &h) != ESP_OK) return def;
    char buf[128];
    size_t len = sizeof(buf);
    esp_err_t e = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return (e == ESP_OK) ? std::string(buf) : def;
}

void HalEsp32::setSettingStr(const char* key, const std::string& value)
{
    nvs_handle_t h;
    if (nvs_open("loadout", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value.c_str());
    nvs_commit(h);
    nvs_close(h);
}

bool HalEsp32::bootFirmware(const std::string& path)
{
    mclog::tagInfo(_tag, "boot firmware: {}", path);
    // Recordar el ultimo firmware lanzado (para marcarlo en el LAUNCHER).
    {
        auto p = path.find_last_of('/');
        setSettingStr("last_fw", (p == std::string::npos) ? path : path.substr(p + 1));
    }
    bsp_sdcard_init((char*)"/sd", 25);
    esp_err_t err = fw_boot_file(path.c_str(), nullptr, nullptr);
    // Solo se llega aqui si falla (en exito reinicia y no retorna).
    bsp_sdcard_deinit((char*)"/sd");
    mclog::error("boot firmware failed: {}", esp_err_to_name(err));
    return false;
}

// Progreso de flasheo: -1 idle, 0..100 flasheando, -2 error.
static volatile int s_flash_pct = -1;
static std::string  s_flash_path;

static void fw_progress_cb(size_t written, size_t total, void* ctx)
{
    s_flash_pct = (total > 0) ? (int)(written * 100 / total) : 0;
}

static void flash_task(void* arg)
{
    s_flash_pct = 0;
    auto p = s_flash_path.find_last_of('/');
    GetHAL()->setSettingStr("last_fw", (p == std::string::npos) ? s_flash_path : s_flash_path.substr(p + 1));
    bsp_sdcard_init((char*)"/sd", 25);
    fw_boot_file(s_flash_path.c_str(), fw_progress_cb, nullptr);  // exito -> reinicia (no retorna)
    bsp_sdcard_deinit((char*)"/sd");
    s_flash_pct = -2;  // solo se llega aqui si fallo
    vTaskDelete(NULL);
}

void HalEsp32::bootFirmwareAsync(const std::string& path)
{
    if (s_flash_pct >= 0) return;  // ya flasheando
    s_flash_path = path;
    s_flash_pct  = 0;
    xTaskCreate(flash_task, "flash", 8192, nullptr, 6, nullptr);
}

int HalEsp32::flashProgress() { return s_flash_pct; }

std::string HalEsp32::readTextFile(const std::string& path)
{
    bsp_sdcard_init((char*)"/sd", 5);
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            out.append(buf, n);
            if (out.size() > 32 * 1024) break;  // limite de seguridad
        }
        fclose(f);
    }
    bsp_sdcard_deinit((char*)"/sd");
    return out;
}

bool HalEsp32::writeTextFile(const std::string& path, const std::string& content)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = false;
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        ok = fwrite(content.data(), 1, content.size(), f) == content.size();
        fclose(f);
    }
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

// Borrado recursivo (sobre /sd ya montada).
static bool rm_recursive(const std::string& full)
{
    struct stat st;
    if (stat(full.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(full.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                rm_recursive(full + "/" + n);
            }
            closedir(d);
        }
        return rmdir(full.c_str()) == 0;
    }
    return remove(full.c_str()) == 0;
}

bool HalEsp32::deletePath(const std::string& path)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = rm_recursive(path);
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

bool HalEsp32::makeDir(const std::string& path)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = (mkdir(path.c_str(), 0775) == 0);
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

bool HalEsp32::createFile(const std::string& path)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = false;
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { ok = true; fclose(f); }
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

bool HalEsp32::copyFile(const std::string& src, const std::string& dst)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = false;
    FILE* in = fopen(src.c_str(), "rb");
    if (in) {
        FILE* out = fopen(dst.c_str(), "wb");
        if (out) {
            char buf[1024];
            size_t n;
            ok = true;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
            }
            fclose(out);
        }
        fclose(in);
    }
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

bool HalEsp32::movePath(const std::string& src, const std::string& dst)
{
    bsp_sdcard_init((char*)"/sd", 5);
    bool ok = (rename(src.c_str(), dst.c_str()) == 0);
    bsp_sdcard_deinit((char*)"/sd");
    return ok;
}

bool HalEsp32::isSdCardMounted()
{
    return true;
}

std::vector<hal::HalBase::FileEntry_t> HalEsp32::scanSdCard(const std::string& dirPath)
{
    std::vector<hal::HalBase::FileEntry_t> file_entries;

    mclog::tagInfo(_tag, "init sd card");
    if (bsp_sdcard_init("/sd", 25) != ESP_OK) {
        mclog::error("failed to mount sd card");
        return file_entries;
    }

    std::string target_path = "/sd/" + dirPath;

    DIR* dir = opendir(target_path.c_str());
    if (dir == nullptr) {
        mclog::error("failed to open directory: {}", target_path);
        return file_entries;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::string(entry->d_name) == "." || std::string(entry->d_name) == "..") {
            continue;
        }

        hal::HalBase::FileEntry_t file_entry;
        file_entry.name  = entry->d_name;
        file_entry.isDir = (entry->d_type == DT_DIR);
        file_entries.push_back(file_entry);
    }

    closedir(dir);

    mclog::tagInfo(_tag, "deinit sd card");
    bsp_sdcard_deinit("/sd");

    return file_entries;
}

/* -------------------------------------------------------------------------- */
/*                                  Interface                                 */
/* -------------------------------------------------------------------------- */
bool HalEsp32::usbCDetect()
{
    return bsp_usb_c_detect();
    // return false;
}

bool HalEsp32::headPhoneDetect()
{
    return bsp_headphone_detect();
}

std::vector<uint8_t> HalEsp32::i2cScan(bool isInternal)
{
    i2c_master_bus_handle_t i2c_bus_handle;
    std::vector<uint8_t> addrs;

    if (isInternal) {
        i2c_bus_handle = bsp_i2c_get_handle();
    } else {
        i2c_bus_handle = bsp_ext_i2c_get_handle();
    }

    esp_err_t ret;
    uint8_t address;

    for (int i = 16; i < 128; i += 16) {
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            ret     = i2c_master_probe(i2c_bus_handle, address, 50);
            if (ret == ESP_OK) {
                addrs.push_back(address);
            }
        }
    }

    return addrs;
}

void HalEsp32::initPortAI2c()
{
    mclog::tagInfo(_tag, "init port a i2c");
    bsp_ext_i2c_init();
}

void HalEsp32::deinitPortAI2c()
{
    mclog::tagInfo(_tag, "deinit port a i2c");
    bsp_ext_i2c_deinit();
}

void HalEsp32::gpioInitOutput(uint8_t pin)
{
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}

void HalEsp32::gpioSetLevel(uint8_t pin, bool level)
{
    gpio_set_level((gpio_num_t)pin, level);
}

void HalEsp32::gpioReset(uint8_t pin)
{
    gpio_set_level((gpio_num_t)pin, false);
}
