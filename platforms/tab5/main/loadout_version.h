#pragma once

// Version del firmware LOADOUT. La CI de GitHub la sobreescribe con el tag
// (ej. tag v1.2.0 -> "1.2.0") al publicar una Release; en local es la de aqui.
// El check de OTA compara esta cadena con la "version" del manifest latest.json.
#ifndef LOADOUT_VERSION
#define LOADOUT_VERSION "1.0.0"
#endif
