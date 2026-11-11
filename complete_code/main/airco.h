#pragma once

#include <stddef.h>
#include "esp_err.h"

#define AIRCO_MAX_JSON_BYTES 4096

// JSON is alleen geldig tijdens deze callback.
typedef void (*airco_reply_fn)(const char *json, void *context);

esp_err_t airco_start(void);

// Kopieert één commando naar de wachtrij zonder te wachten op IR-verzending of flashopslag.
esp_err_t airco_submit_json(const char *json, size_t length, airco_reply_fn reply,
                                  void *context);
