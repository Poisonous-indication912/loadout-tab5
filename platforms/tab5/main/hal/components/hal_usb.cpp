/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <driver/gpio.h>
#include <memory>
#include <mutex>
#include <lvgl.h>
#include <usb/usb_host.h>
#include <usb/hid_host.h>
#include <usb/hid_usage_keyboard.h>
#include <usb/hid_usage_mouse.h>
#include <esp_log.h>
#include <assets/assets.h>
#include <bsp/m5stack_tab5.h>
#include <driver/i2c_master.h>
#include <cstring>

#define TAG "usba"

static std::mutex _usba_detect_mutex;
static bool _is_usba_connected = false;
static bool _is_keyboard_connected = false;
static lv_obj_t* _cursor_img;
static QueueHandle_t kb_queue = nullptr;  // teclas traducidas (LVGL keys) HID->LVGL

QueueHandle_t app_event_queue = NULL;
typedef enum { APP_EVENT = 0, APP_EVENT_HID_HOST } app_event_group_t;

typedef struct {
    app_event_group_t event_group;
    struct {
        hid_host_device_handle_t handle;
        hid_host_driver_event_t event;
        void* arg;
    } hid_host_device;
} app_event_queue_t;

static const char* hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

typedef struct {
    enum key_state { KEY_STATE_PRESSED = 0x00, KEY_STATE_RELEASED = 0x01 } state;
    uint8_t modifier;
    uint8_t key_code;
} key_event_t;

#define KEYBOARD_ENTER_MAIN_CHAR '\r'
#define KEYBOARD_ENTER_LF_EXTEND 1

static void hid_print_new_device_report_header(hid_protocol_t proto)
{
    static hid_protocol_t prev_proto_output;

    if (prev_proto_output != proto) {
        prev_proto_output = proto;
        printf("\r\n");
        if (proto == HID_PROTOCOL_MOUSE) {
            printf("Mouse\r\n");
        } else if (proto == HID_PROTOCOL_KEYBOARD) {
            printf("Keyboard\r\n");
        } else {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}

// Traduce un keycode HID (boot keyboard) a una tecla LVGL (ASCII o LV_KEY_*).
static uint32_t hid_kc_to_lvkey(uint8_t kc, bool shift)
{
    if (kc >= 0x04 && kc <= 0x1d) {  // a-z
        char c = 'a' + (kc - 0x04);
        return shift ? (uint32_t)(c - 32) : (uint32_t)c;
    }
    if (kc >= 0x1e && kc <= 0x26) {  // 1-9
        const char* n = "123456789";
        const char* s = "!@#$%^&*(";
        return shift ? (uint32_t)s[kc - 0x1e] : (uint32_t)n[kc - 0x1e];
    }
    switch (kc) {
        case 0x27: return shift ? ')' : '0';
        case 0x2c: return ' ';
        case 0x28: return LV_KEY_ENTER;
        case 0x2a: return LV_KEY_BACKSPACE;
        case 0x2b: return '\t';
        case 0x4f: return LV_KEY_NEXT;   // right -> siguiente (navegacion de grupo)
        case 0x50: return LV_KEY_PREV;   // left  -> anterior
        case 0x51: return LV_KEY_NEXT;   // down  -> siguiente
        case 0x52: return LV_KEY_PREV;   // up    -> anterior
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
    }
    return 0;
}

// Report de teclado HID -> encola las teclas nuevas para LVGL.
static void hid_host_keyboard_report_callback(const uint8_t* const data, const int length)
{
    if (length < 8) {
        return;
    }
    uint8_t modifier = data[0];
    bool    shift    = (modifier & 0x22) != 0;  // L/R shift
    static uint8_t prev[6] = {0};

    for (int i = 2; i < 8; i++) {
        uint8_t kc = data[i];
        if (kc == 0) {
            continue;
        }
        bool already = false;
        for (int j = 0; j < 6; j++) {
            if (prev[j] == kc) { already = true; break; }
        }
        if (already) {
            continue;  // ya estaba pulsada (evitar repeticion)
        }
        uint32_t key = hid_kc_to_lvkey(kc, shift);
        if (key && kb_queue) {
            xQueueSend(kb_queue, &key, 0);
        }
    }
    for (int i = 0; i < 6; i++) {
        prev[i] = data[2 + i];
    }
}

static void hid_host_mouse_report_callback(const uint8_t* const data, const int length)
{
    hid_mouse_input_report_boot_t* mouse_report = (hid_mouse_input_report_boot_t*)data;

    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }

    static int x_pos = 720 / 2;
    static int y_pos = 1280 / 2;

    // Calculate absolute position from displacement
    x_pos += mouse_report->y_displacement;
    y_pos -= mouse_report->x_displacement;

    x_pos = std::clamp(x_pos, 0, 720);
    y_pos = std::clamp(y_pos, 0, 1280);

    hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);

    // printf("X: %06d\tY: %06d\t|%c|%c|\r", x_pos, y_pos, (mouse_report->buttons.button1 ? 'o' : ' '),
    //        (mouse_report->buttons.button2 ? 'o' : ' '));

    GetHAL()->hidMouseData.mutex.lock();
    GetHAL()->hidMouseData.x        = x_pos;
    GetHAL()->hidMouseData.y        = y_pos;
    GetHAL()->hidMouseData.btnLeft  = mouse_report->buttons.button1;
    GetHAL()->hidMouseData.btnRight = mouse_report->buttons.button2;
    GetHAL()->hidMouseData.mutex.unlock();

    fflush(stdout);
}

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event,
                                 void* arg)
{
    uint8_t data[64]   = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));

            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    hid_host_keyboard_report_callback(data, data_length);
                } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    hid_host_mouse_report_callback(data, data_length);
                }
            } else {
                // hid_host_generic_report_callback(data, data_length);
            }

            break;
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));

            _usba_detect_mutex.lock();
            _is_usba_connected = false;
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                _is_keyboard_connected = false;
            }
            _usba_detect_mutex.unlock();

            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR", hid_proto_name_str[dev_params.proto]);
            break;
        default:
            ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event", hid_proto_name_str[dev_params.proto]);
            break;
    }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void* arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);

            const hid_host_device_config_t dev_config = {.callback = hid_host_interface_callback, .callback_arg = NULL};

            ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
                }
            }
            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));

            _usba_detect_mutex.lock();
            _is_usba_connected = true;
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                _is_keyboard_connected = true;
            }
            _usba_detect_mutex.unlock();

            break;
        }
        default:
            break;
    }
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event,
                              void* arg)
{
    const app_event_queue_t evt_queue = {.event_group = APP_EVENT_HID_HOST,
                                         // HID Host Device related info
                                         .hid_host_device = {.handle = hid_device_handle, .event = event, .arg = arg}};

    if (app_event_queue) {
        xQueueSend(app_event_queue, &evt_queue, 0);
    }
}

static void tab5_usb_host_task(void* pvParameters)
{
    // BaseType_t task_created;
    app_event_queue_t evt_queue;
    // ESP_LOGI(TAG, "HID Host example");
    // task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, xTaskGetCurrentTaskHandle(), 2, NULL,
    // 0); assert(task_created == pdTRUE);

    ulTaskNotifyTake(false, 1000);
    const hid_host_driver_config_t hid_host_driver_config = {.create_background_task = true,
                                                             .task_priority          = 5,
                                                             .stack_size             = 4096,
                                                             .core_id                = 0,
                                                             .callback               = hid_host_device_callback,
                                                             .callback_arg           = NULL};

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    // Create queue
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));

    ESP_LOGI(TAG, "Waiting for HID Device to be connected");

    while (1) {
        // Wait queue
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT == evt_queue.event_group) {
                // User pressed button
                usb_host_lib_info_t lib_info;
                ESP_ERROR_CHECK(usb_host_lib_info(&lib_info));
                if (lib_info.num_devices == 0) {
                    // End while cycle
                    break;
                } else {
                    ESP_LOGW(TAG, "To shutdown example, remove all USB devices and press button again.");
                    // Keep polling
                }
            }

            if (APP_EVENT_HID_HOST == evt_queue.event_group) {
                hid_host_device_event(evt_queue.hid_host_device.handle, evt_queue.hid_host_device.event,
                                      evt_queue.hid_host_device.arg);
            }
        }
    }
}

static void lvgl_mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    _usba_detect_mutex.lock();
    if (!_is_usba_connected) {
        _usba_detect_mutex.unlock();
        data->state = LV_INDEV_STATE_REL;
        if (lv_obj_get_style_opa(_cursor_img, LV_PART_MAIN) == LV_OPA_COVER) {
            lv_obj_set_style_opa(_cursor_img, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        return;
    }
    _usba_detect_mutex.unlock();
    if (lv_obj_get_style_opa(_cursor_img, LV_PART_MAIN) == LV_OPA_TRANSP) {
        lv_obj_set_style_opa(_cursor_img, LV_OPA_COVER, LV_PART_MAIN);
    }

    std::lock_guard<std::mutex> lock(GetHAL()->hidMouseData.mutex);
    data->point.x = GetHAL()->hidMouseData.x;
    data->point.y = GetHAL()->hidMouseData.y;
    data->state   = GetHAL()->hidMouseData.btnLeft ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Lee teclas encoladas por el teclado HID y las entrega a LVGL (keypad indev).
static void lvgl_kb_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    static uint32_t last = 0;
    uint32_t k;
    if (kb_queue && xQueueReceive(kb_queue, &k, 0) == pdTRUE) {
        last        = k;
        data->key   = k;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key   = last;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

#define KB_SMART_ADDR 0x6D   // direccion por defecto del teclado smart
#define KB_TCA_ADDR   0x34   // direccion alternativa (la que reporta tu unidad)

static i2c_master_dev_handle_t s_smart_dev = nullptr;

static bool smart_read(uint8_t reg, uint8_t* out, size_t n)
{
    if (!s_smart_dev) return false;
    return i2c_master_transmit_receive(s_smart_dev, &reg, 1, out, n, 50) == ESP_OK;
}
static bool smart_write(uint8_t reg, uint8_t val)
{
    if (!s_smart_dev) return false;
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_smart_dev, b, 2, 50) == ESP_OK;
}

// ASCII -> tecla LVGL (teclado smart, modo STRING).
// Flechas del teclado Tab5 (modo STRING): UP=0x11 DOWN=0x12 RIGHT=0x13 LEFT=0x14.
// Para navegar el grupo LVGL se traducen a NEXT/PREV (la navegacion de grupo es lineal).
static uint32_t char_to_lvkey(uint8_t c)
{
    if (c == '\r' || c == '\n') return LV_KEY_ENTER;
    if (c == 0x08 || c == 0x7F) return LV_KEY_BACKSPACE;
    if (c == '\t')              return LV_KEY_NEXT;
    if (c == 27)                return LV_KEY_ESC;
    if (c == 0x11)              return LV_KEY_PREV;   // UP
    if (c == 0x14)              return LV_KEY_PREV;   // LEFT
    if (c == 0x12)              return LV_KEY_NEXT;   // DOWN
    if (c == 0x13)              return LV_KEY_NEXT;   // RIGHT
    if (c >= 0x20 && c < 0x7F)  return (uint32_t)c;
    return 0;
}

// Teclas especiales que el teclado Tab5 (modo STRING) manda como PALABRA ASCII
// en un solo evento: "left","right","up","down","enter","esc","space","tab",
// "del","backspace". Las teclas normales llegan de 1 solo caracter.
static uint32_t word_to_lvkey(const char* in)
{
    // Normalizar a minusculas (la unidad podria mandar "Esc"/"ESC"/"esc").
    char s[18];
    int i = 0;
    for (; in[i] && i < 16; i++) s[i] = (in[i] >= 'A' && in[i] <= 'Z') ? in[i] + 32 : in[i];
    s[i] = 0;

    // Flechas -> teclas de direccion reales (la navegacion 2D la hace nav_key_cb;
    // en un textarea mueven el cursor).
    if (!strcmp(s, "left"))                          return LV_KEY_LEFT;
    if (!strcmp(s, "right"))                         return LV_KEY_RIGHT;
    if (!strcmp(s, "up"))                            return LV_KEY_UP;
    if (!strcmp(s, "down"))                          return LV_KEY_DOWN;
    if (!strcmp(s, "enter") || !strcmp(s, "return")) return LV_KEY_ENTER;
    if (!strcmp(s, "esc")   || !strcmp(s, "escape")) return LV_KEY_ESC;
    if (!strcmp(s, "tab"))                           return LV_KEY_NEXT;
    if (!strcmp(s, "space"))                         return (uint32_t)' ';
    if (!strcmp(s, "home"))                          return LV_KEY_HOME;
    if (!strcmp(s, "end"))                           return LV_KEY_END;
    if (!strcmp(s, "del") || !strcmp(s, "delete") ||
        !strcmp(s, "backspace") || !strcmp(s, "bksp"))
        return LV_KEY_BACKSPACE;
    return 0;  // palabra desconocida -> ignorar
}

static void kb_set_connected(bool v, int addr)
{
    _usba_detect_mutex.lock();
    _is_keyboard_connected = v;
    _usba_detect_mutex.unlock();
    GetHAL()->dbgKbAddr = v ? addr : 0;
}

// Tarea del teclado: el STM32 del teclado habla el protocolo SMART (modo STRING
// -> ASCII), y su direccion puede ser 0x6D o 0x34 segun unidad/firmware. Probamos
// ambas en ambos buses, SIN inundar (probe + backoff 1s).
static void tab5_keypad_task(void* arg)
{
    bsp_ext_i2c_init();
    i2c_master_bus_handle_t ext_bus = bsp_ext_i2c_get_handle();
    i2c_master_bus_handle_t int_bus = bsp_i2c_get_handle();
    struct Cand { i2c_master_bus_handle_t bus; uint8_t addr; };
    Cand cands[] = {{ext_bus, KB_SMART_ADDR}, {ext_bus, KB_TCA_ADDR},
                    {int_bus, KB_SMART_ADDR}, {int_bus, KB_TCA_ADDR}};
    bool connected = false;
    int  fails     = 0;

    while (1) {
        if (!connected) {
            // Deteccion por LECTURA real (no por probe): añade el dispositivo y prueba
            // a leer EVENT_NUM. Si hace ACK, esta ahi de verdad.
            for (auto& c : cands) {
                if (s_smart_dev) { i2c_master_bus_rm_device(s_smart_dev); s_smart_dev = nullptr; }
                i2c_device_config_t cfg = {};
                cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
                cfg.device_address  = c.addr;
                cfg.scl_speed_hz    = 100000;
                if (i2c_master_bus_add_device(c.bus, &cfg, &s_smart_dev) != ESP_OK) {
                    s_smart_dev = nullptr;
                    continue;
                }
                uint8_t tmp = 0;
                if (smart_read(0x02, &tmp, 1)) {   // EVENT_NUM ACK => presente
                    smart_write(0x10, 2);          // modo STRING -> ASCII
                    connected = true; fails = 0;
                    kb_set_connected(true, c.addr);
                    ESP_LOGI(TAG, "Teclado detectado @0x%02X (bus %s)",
                             c.addr, c.bus == ext_bus ? "ext" : "int");
                    break;
                }
                i2c_master_bus_rm_device(s_smart_dev);
                s_smart_dev = nullptr;
            }
            if (!connected) {
                kb_set_connected(false, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));  // sin teclado: reintento lento
                continue;
            }
        }

        uint8_t count = 0;
        if (!smart_read(0x02, &count, 1)) {  // EVENT_NUM; NACK repetido => re-detectar
            if (++fails > 3) { connected = false; kb_set_connected(false, 0); }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        fails = 0;
        for (int i = 0; i < count && i < 32; i++) {
            uint8_t len = 0;
            if (smart_read(0x40, &len, 1) && len > 0 && len <= 15) {
                uint8_t buf[17] = {0};
                if (smart_read(0x50, buf, len + 1)) {  // buf[0]=modifier, buf[1..]=ASCII (palabra+null)
                    // Montar la cadena (buf[1..] hasta el null).
                    char s[18];
                    int n = 0;
                    for (int j = 1; j <= len && n < 16 && buf[j] != 0; j++) s[n++] = (char)buf[j];
                    s[n] = 0;
                    GetHAL()->dbgKeyCount++;
                    GetHAL()->dbgLastKey = (n > 0) ? (uint8_t)s[0] : 0;

                    uint32_t lk = 0;
                    if (n == 1) {
                        lk = char_to_lvkey((uint8_t)s[0]);      // caracter normal o control de 1 byte
                    } else if (n > 1) {
                        lk = word_to_lvkey(s);                  // tecla especial como palabra: "left","up"...
                    }
                    if (lk && kb_queue) xQueueSend(kb_queue, &lk, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void HalEsp32::hid_init()
{
    mclog::tagInfo(TAG, "hid init");
    xTaskCreatePinnedToCore(tab5_usb_host_task, "usba", 4096 * 2, NULL, 5, NULL, 0);

    auto lvMouse = lv_indev_create();
    lv_indev_set_type(lvMouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvMouse, lvgl_mouse_read_cb);
    lv_indev_set_display(lvMouse, lvDisp);

    _cursor_img = lv_image_create(lv_screen_active()); /*Create an image object for the cursor */
    lv_image_set_src(_cursor_img, &mouse_cursor);      /*Set the image source*/
    lv_indev_set_cursor(lvMouse, _cursor_img);         /*Connect the image  object to the driver*/

    // Teclado: indev tipo KEYPAD alimentado por la cola HID.
    kb_queue   = xQueueCreate(32, sizeof(uint32_t));
    lvKeyboard = lv_indev_create();
    lv_indev_set_type(lvKeyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(lvKeyboard, lvgl_kb_read_cb);
    lv_indev_set_display(lvKeyboard, lvDisp);

    // Teclado matricial TCA8418 en el puerto externo (ext.port1).
    xTaskCreatePinnedToCore(tab5_keypad_task, "keypad", 4096, NULL, 5, NULL, 0);
}

bool HalEsp32::usbADetect()
{
    std::lock_guard<std::mutex> lock(_usba_detect_mutex);
    return _is_usba_connected;
}

bool HalEsp32::isKeyboardConnected()
{
    std::lock_guard<std::mutex> lock(_usba_detect_mutex);
    return _is_keyboard_connected;
}
