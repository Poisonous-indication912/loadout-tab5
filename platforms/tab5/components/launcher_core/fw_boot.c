#include "fw_boot.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdint.h>

static const char *TAG = "fw_boot";

#define FW_CHUNK 4096

esp_err_t fw_boot_file(const char *path, fw_boot_progress_cb_t progress, void *ctx)
{
    // Ranura de app LIBRE (la que NO esta corriendo el launcher). Con dos slots
    // (ota_0/ota_1) esto alterna; el externo se escribe ahi y, al no marcarse
    // valido, el siguiente reset hace rollback a la ranura del launcher.
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "No hay particion OTA libre");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Destino: %s", target->label);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s", path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (size_t)size > target->size) {
        ESP_LOGE(TAG, "Tamano invalido (%ld bytes, ota_0=%u)", size, (unsigned)target->size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Escribiendo %ld bytes de %s en ota_0...", size, path);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, (size_t)size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        fclose(f);
        return err;
    }

    static uint8_t buf[FW_CHUNK];
    size_t written = 0, r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            break;
        }
        written += r;
        if (progress) {
            progress(written, (size_t)size, ctx);
        }
    }
    fclose(f);

    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }

    err = esp_ota_end(handle);  // valida magic / chip id / revision
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Imagen invalida (esp_ota_end): %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Firmware listo. Reiniciando en ota_0...");
    esp_restart();
    return ESP_OK;  // inalcanzable
}
