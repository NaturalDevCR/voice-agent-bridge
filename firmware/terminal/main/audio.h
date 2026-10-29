// Audio del AtomS3R + Atomic Audio-3.5 Base: ES8311 duplex a 16 kHz mono s16le.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE          16000
#define AUDIO_FRAME_SAMPLES 320  // 20 ms

// Se llama desde la tarea del micrófono con cada trama de 20 ms.
typedef void (*audio_mic_cb_t)(const int16_t *pcm, size_t samples);

typedef struct {
    uint32_t overruns;    // bytes de reproducción descartados por búfer lleno (eventos)
    uint32_t drains;      // veces que el búfer de reproducción se vació mientras sonaba
    float mic_dbfs;       // RMS del micrófono, último segundo
    size_t play_ms;       // audio pendiente de reproducir
} audio_stats_t;

bool audio_init(int volume, int gain_db);
void audio_start(audio_mic_cb_t cb);
void audio_set_volume(int volume);
void audio_set_gain(int gain_db);
// Ecualizador de salida: estante de graves (250 Hz) y de agudos (3 kHz), en dB.
// Compensa la salida del jack de la base, que con audífonos suena sin graves.
void audio_set_eq(int bass_db, int treble_db);
// Silencio del DAC. AUTO (por defecto) silencia cuando no suena nada: quita el siseo de fondo
// del jack, que el Hito 2 confirmó que viene de la salida analógica en reposo.
typedef enum { AUDIO_MUTE_AUTO, AUDIO_MUTE_ON, AUDIO_MUTE_OFF } audio_mute_t;
void audio_set_mute(audio_mute_t mode);
// PW_AMP (P0 del PI4IOE): el silencio automático lo apaga junto con el DAC.
void audio_set_amp(bool on);

// Reproducción: PCM s16le en un búfer circular en PSRAM, sin importar el tamaño de cada trozo.
size_t audio_play_push(const uint8_t *data, size_t len);
void audio_play_flush(void);
void audio_beep(float hz, int ms, int count);
// true mientras suena audio o durante la cola posterior (half-duplex E0).
bool audio_playing(void);
void audio_get_stats(audio_stats_t *out);
