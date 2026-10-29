// Pantalla 128×128 del AtomS3R: logo al arrancar y luego estado de conexión, conversación y estabilidad.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISPLAY_NO_CONFIG,  // falta Wi-Fi, URL o token
    DISPLAY_NO_WIFI,
    DISPLAY_NO_BRIDGE,  // Wi-Fi OK, WebSocket caído
    DISPLAY_ONLINE,
} display_link_t;

typedef struct {
    display_link_t link;
    const char *state;     // estado del puente: idle, listening, speaking, …
    bool playing;          // suena audio en el terminal
    int rssi;              // dBm (0 = sin dato)
    int channel;
    uint32_t wifi_drops;   // cortes de Wi-Fi desde el arranque
    uint32_t bridge_drops; // cortes del WebSocket desde el arranque
    int64_t online_s;      // segundos de la conexión actual al puente
} display_info_t;

// Inicia panel, retroiluminación y LVGL; muestra el logo con `name`. false si no hay pantalla.
bool display_init(const char *name, const char *version);
// Retroiluminación 0–100 %. Entre 1 y 99 el LP5562 conmuta a 558 Hz (PWM).
void display_set_brightness(int percent);
// Actualiza la pantalla principal (llamar periódicamente; sólo redibuja lo que cambia).
void display_show(const display_info_t *info);
