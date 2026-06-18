// fw_boot — arrancar un firmware destino desde un .bin (capa CORE portable).
// Copia el .bin a la partición de aplicación 'ota_0', valida la imagen, la
// marca como arranque y reinicia. Con rollback OTA activado, el siguiente
// reset vuelve al launcher (factory). Agnóstico de la ruta: pásale "/sd/x.bin".

#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fw_boot_progress_cb_t)(size_t written, size_t total, void *ctx);

// Escribe el .bin de `path` en ota_0, valida, marca arranque y reinicia
// (no retorna si tiene éxito). Si falla, retorna el error y NO reinicia.
esp_err_t fw_boot_file(const char *path, fw_boot_progress_cb_t progress, void *ctx);

#ifdef __cplusplus
}
#endif
