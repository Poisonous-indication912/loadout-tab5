/*
 * Grabador de pantalla del dispositivo: captura el framebuffer LVGL con
 * lv_snapshot, lo comprime con el encoder JPEG por HARDWARE del ESP32-P4 y
 * escribe una secuencia de .jpg en la microSD (/sd/rec/fNNNNN.jpg).
 * Luego en el PC: ffmpeg -framerate 8 -i /ruta/rec/f%05d.jpg salida.gif
 * (o tools/frames2gif.sh). v1 robusta; el empaquetado a AVI de un fichero es
 * un paso posterior una vez validados los frames.
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <esp_log.h>
#include <lvgl.h>
#include <driver/jpeg_encode.h>
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string>

#define TAG "screenrec"

static volatile bool s_rec_active = false;
static volatile bool s_rec_stop   = false;
static volatile int  s_rec_frames = 0;
static const int     REC_FPS      = 8;

static void rec_task(void* arg)
{
    const int W = 1280, H = 720;
    const size_t IN_SZ  = (size_t)W * H * 2;     // RGB565
    size_t in_alloc = 0, out_alloc = 0;
    uint8_t* in_buf  = nullptr;
    uint8_t* out_buf = nullptr;
    jpeg_encoder_handle_t enc = nullptr;
    bool sd = false;

    jpeg_encode_engine_cfg_t eng = {};
    eng.timeout_ms = 120;
    if (jpeg_new_encoder_engine(&eng, &enc) != ESP_OK) { ESP_LOGE(TAG, "enc engine fail"); goto done; }

    {
        jpeg_encode_memory_alloc_cfg_t mi = {}; mi.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
        in_buf = (uint8_t*)jpeg_alloc_encoder_mem(IN_SZ, &mi, &in_alloc);
        jpeg_encode_memory_alloc_cfg_t mo = {}; mo.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
        out_buf = (uint8_t*)jpeg_alloc_encoder_mem(768 * 1024, &mo, &out_alloc);
    }
    if (!in_buf || !out_buf) { ESP_LOGE(TAG, "jpeg mem fail"); goto done; }

    if (bsp_sdcard_init((char*)"/sd", 25) != ESP_OK) { ESP_LOGE(TAG, "no SD"); goto done; }
    sd = true;
    mkdir("/sd/rec", 0775);

    s_rec_frames = 0;
    while (!s_rec_stop) {
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);

        // 1) snapshot de la pantalla activa (bajo lock LVGL). No incluye la
        //    top-layer -> el indicador REC no se graba.
        GetHAL()->lvglLock();
        lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        if (snap) {
            uint32_t stride = snap->header.stride;
            const uint8_t* src = snap->data;
            for (int y = 0; y < H; y++) memcpy(in_buf + (size_t)y * W * 2, src + (size_t)y * stride, W * 2);
        }
        GetHAL()->lvglUnlock();
        if (!snap) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        lv_draw_buf_destroy(snap);

        // 2) JPEG por hardware
        jpeg_encode_cfg_t cfg = {};
        cfg.width = W; cfg.height = H;
        cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
        cfg.image_quality = 80;
        uint32_t jlen = 0;
        if (jpeg_encoder_process(enc, &cfg, in_buf, IN_SZ, out_buf, out_alloc, &jlen) == ESP_OK && jlen > 0) {
            char path[40];
            snprintf(path, sizeof(path), "/sd/rec/f%05d.jpg", s_rec_frames);
            FILE* f = fopen(path, "wb");
            if (f) { fwrite(out_buf, 1, jlen, f); fclose(f); s_rec_frames++; }
        }

        // 3) ritmo ~REC_FPS
        uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        int wait = 1000 / REC_FPS - (int)dt;
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    }

done:
    if (sd) bsp_sdcard_deinit((char*)"/sd");
    if (enc) jpeg_del_encoder_engine(enc);
    if (in_buf) free(in_buf);
    if (out_buf) free(out_buf);
    ESP_LOGI(TAG, "rec stopped (%d frames)", s_rec_frames);
    s_rec_active = false;
    s_rec_stop   = false;
    vTaskDelete(NULL);
}

bool HalEsp32::screenRecToggle()
{
    if (s_rec_active) { s_rec_stop = true; return false; }
    s_rec_active = true; s_rec_stop = false; s_rec_frames = 0;
    xTaskCreatePinnedToCore(rec_task, "screenrec", 8192, nullptr, 4, nullptr, 1);
    return true;
}
bool HalEsp32::screenRecActive() { return s_rec_active; }
int  HalEsp32::screenRecFrames() { return s_rec_frames; }
