#pragma once

#include <stddef.h>

typedef struct {
    const char *id;
    const char *description;
    const char *nvs_key;
} ac_preset_t;

static const ac_preset_t AC_PRESETS[] = {

    {"koelen18eco", "Koelen, 18 graden, eco", "cool18eco"},
    {"koelen18auto", "Koelen, 18 graden, auto", "cool18"},
    {"koelen18strong", "Koelen, 18 graden, strong", "cool18strong"},
    {"koelen19eco", "Koelen, 19 graden, eco", "cool19eco"},
    {"koelen19auto", "Koelen, 19 graden, auto", "preset0"},
    {"koelen19strong", "Koelen, 19 graden, strong", "cool19strong"},
    {"koelen20eco", "Koelen, 20 graden, eco", "cool20eco"},
    {"koelen20auto", "Koelen, 20 graden, auto", "preset1"},
    {"koelen20strong", "Koelen, 20 graden, strong", "cool20strong"},
    {"verwarmen20eco", "Verwarmen, 20 graden, eco", "heat20eco"},
    {"verwarmen20auto", "Verwarmen, 20 graden, auto", "heat20auto"},
    {"verwarmen20strong", "Verwarmen, 20 graden, strong", "heat20strong"},
    {"verwarmen21eco", "Verwarmen, 21 graden, eco", "heat21eco"},
    {"verwarmen21auto", "Verwarmen, 21 graden, auto", "heat21auto"},
    {"verwarmen21strong", "Verwarmen, 21 graden, strong", "heat21strong"},
    {"verwarmen22eco", "Verwarmen, 22 graden, eco", "heat22eco"},
    {"verwarmen22auto", "Verwarmen, 22 graden, auto", "heat22auto"},
    {"verwarmen22strong", "Verwarmen, 22 graden, strong", "heat22strong"},
    {"verwarmen23eco", "Verwarmen, 23 graden, eco", "heat23eco"},
    {"verwarmen23auto", "Verwarmen, 23 graden, auto", "heat23auto"},
    {"verwarmen23strong", "Verwarmen, 23 graden, strong", "heat23strong"},
    {"uit", "Airco uit", "uit"},
};

#define AC_PRESET_COUNT (sizeof(AC_PRESETS) / sizeof(AC_PRESETS[0]))

