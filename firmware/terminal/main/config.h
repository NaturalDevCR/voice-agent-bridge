// Configuración del terminal guardada en NVS (se edita desde la consola USB).
#pragma once

#include <stdbool.h>

typedef struct {
    char ssid[33];
    char pass[65];
    char url[160];    // ws://<puente>:8765/v0/terminal?id=<nombre>
    char token[97];
    char name[32];    // se muestra en el logo (sin tildes: la fuente es ASCII)
    int volume;       // 0–100
    int gain_db;      // ganancia del micrófono
    int tx_power;     // dBm, potencia máxima de Wi-Fi (20 = máximo alcance; menos = menos ruido en el audio)
    int eq_bass;      // dB, estante de graves 250 Hz
    int eq_treble;    // dB, estante de agudos 3 kHz
} term_config_t;

extern term_config_t g_cfg;

void config_load(void);
bool config_save(void);
bool config_ready(void);  // hay Wi-Fi, URL y token
void config_print(void);
