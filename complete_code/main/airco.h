#pragma once

#include <stddef.h>
#include "esp_err.h"

#define AIRCO_MAX_JSON_BYTES 4096

// Called from the command worker. JSON is valid only during this callback.
typedef void (*airco_reply_fn)(const char *json, void *context);

esp_err_t airco_start(void);

// Copies and queues one command without waiting for IR transmission or flash.
esp_err_t airco_submit_json(const char *json, size_t length, airco_reply_fn reply,
                                  void *context);
