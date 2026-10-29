// Inicialización y tareas de audio. Pinout y ajustes validados en el Hito 1 (docs/03).

#include "audio.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define PIN_I2S_WS    GPIO_NUM_6
#define PIN_I2S_BCLK  GPIO_NUM_8
#define PIN_I2S_DOUT  GPIO_NUM_5
#define PIN_I2S_DIN   GPIO_NUM_7
#define PIN_I2C_SDA   GPIO_NUM_38
#define PIN_I2C_SCL   GPIO_NUM_39

#define PI4IOE_ADDR        0x43
#define PI4IOE_REG_IO_DIR  0x03
#define PI4IOE_REG_IO_OUT  0x05
#define PI4IOE_REG_IO_HIZ  0x07
#define PI4IOE_REG_PULLUP  0x0D

#define RING_BYTES     (30 * AUDIO_RATE * 2)  // 30 s
#define PREBUFFER_MS   100
#define PREBUFFER_WAIT_MS 150
#define TAIL_MS        250

static esp_codec_dev_handle_t s_codec;
static i2c_master_dev_handle_t s_pi4ioe;
static audio_mic_cb_t s_mic_cb;

static uint8_t *s_ring;
static size_t s_head, s_tail;  // escribe en head, lee en tail
static SemaphoreHandle_t s_ring_lock;
static int64_t s_first_byte_us;  // cuándo llegó audio con el reproductor en reposo
static volatile int64_t s_last_play_us;
static volatile bool s_playing;
static volatile uint32_t s_overruns, s_drains;
static volatile float s_mic_dbfs = -120;

// ---------------------------------------------------------------- hardware

static void pi4ioe_init(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IOE_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return;
    }
    const uint8_t regs[][2] = {
        {PI4IOE_REG_IO_HIZ, 0x00},
        {PI4IOE_REG_PULLUP, 0xFF},
        {PI4IOE_REG_IO_DIR, 0x6F},  // xiaozhi (0x6E) + P0 = PW_AMP como salida
        {PI4IOE_REG_IO_OUT, 0xFF},
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        i2c_master_transmit(dev, regs[i], 2, 100);
    }
    s_pi4ioe = dev;
}

void audio_set_amp(bool on)
{
    if (!s_pi4ioe) {
        return;
    }
    uint8_t reg = PI4IOE_REG_IO_OUT, out = 0xFF;
    i2c_master_transmit_receive(s_pi4ioe, &reg, 1, &out, 1, 100);
    uint8_t buf[2] = {PI4IOE_REG_IO_OUT, on ? (out | 0x01) : (out & ~0x01)};
    i2c_master_transmit(s_pi4ioe, buf, 2, 100);
}

static volatile audio_mute_t s_mute_mode = AUDIO_MUTE_AUTO;

// La tarea de reproducción aplica el cambio: así el DAC sólo se toca entre tramas.
void audio_set_mute(audio_mute_t mode)
{
    s_mute_mode = mode;
}

bool audio_init(int volume, int gain_db)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    s_ring_lock = xSemaphoreCreateMutex();
    if (!s_ring) {
        ESP_LOGE(TAG, "sin PSRAM para el búfer de reproducción");
        return false;
    }

    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    if (i2c_master_probe(bus, 0x18, 50) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 no responde: ¿está el Atom bien asentado en la base?");
        return false;
    }
    pi4ioe_init(bus);

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &tx, &rx));
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_NUM_1, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_codec_new(&es_cfg),
        .data_if = audio_codec_new_i2s_data(&i2s_cfg),
    };
    if (!dev_cfg.codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new falló");
        return false;
    }
    s_codec = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec, &fs));
    audio_set_volume(volume);
    audio_set_gain(gain_db);
    ESP_LOGI(TAG, "ES8311 listo: %d Hz, volumen %d, ganancia %d dB", AUDIO_RATE, volume, gain_db);
    return true;
}

void audio_set_volume(int volume)
{
    if (s_codec) {
        esp_codec_dev_set_out_vol(s_codec, volume);
    }
}

void audio_set_gain(int gain_db)
{
    if (s_codec) {
        esp_codec_dev_set_in_gain(s_codec, gain_db);
    }
}

// ---------------------------------------------------------------- búfer de reproducción

static size_t ring_used(void)
{
    return (s_head + RING_BYTES - s_tail) % RING_BYTES;
}

size_t audio_play_push(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t room = RING_BYTES - 1 - ring_used();
    if (len > room) {
        s_overruns++;
        len = room;
    }
    if (ring_used() == 0 && !s_playing) {
        s_first_byte_us = esp_timer_get_time();
    }
    size_t first = len < RING_BYTES - s_head ? len : RING_BYTES - s_head;
    memcpy(s_ring + s_head, data, first);
    memcpy(s_ring, data + first, len - first);
    s_head = (s_head + len) % RING_BYTES;
    xSemaphoreGive(s_ring_lock);
    return len;
}

void audio_play_flush(void)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_tail = s_head;
    xSemaphoreGive(s_ring_lock);
}

static size_t ring_pop(uint8_t *dst, size_t len)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t used = ring_used();
    len = (len < used ? len : used) & ~(size_t)1;  // muestras completas
    size_t first = len < RING_BYTES - s_tail ? len : RING_BYTES - s_tail;
    memcpy(dst, s_ring + s_tail, first);
    memcpy(dst + first, s_ring, len - first);
    s_tail = (s_tail + len) % RING_BYTES;
    xSemaphoreGive(s_ring_lock);
    return len;
}

void audio_beep(float hz, int ms, int count)
{
    int16_t buf[AUDIO_FRAME_SAMPLES];
    float phase = 0, step = 2.0f * (float)M_PI * hz / AUDIO_RATE, amp = 32767.0f * 0.1f;  // −20 dBFS
    for (int c = 0; c < count; c++) {
        int total = AUDIO_RATE * ms / 1000;
        for (int done = 0; done < total; done += AUDIO_FRAME_SAMPLES) {
            int n = total - done < AUDIO_FRAME_SAMPLES ? total - done : AUDIO_FRAME_SAMPLES;
            for (int i = 0; i < n; i++) {
                buf[i] = (int16_t)(amp * sinf(phase));
                phase += step;
            }
            audio_play_push((uint8_t *)buf, n * sizeof(int16_t));
        }
        memset(buf, 0, sizeof(buf));
        for (int gap = 0; gap < 4; gap++) {  // 80 ms entre pitidos
            audio_play_push((uint8_t *)buf, sizeof(buf));
        }
    }
}

bool audio_playing(void)
{
    return s_playing || esp_timer_get_time() - s_last_play_us < TAIL_MS * 1000;
}

void audio_get_stats(audio_stats_t *out)
{
    out->overruns = s_overruns;
    out->drains = s_drains;
    out->mic_dbfs = s_mic_dbfs;
    out->play_ms = ring_used() / 2 * 1000 / AUDIO_RATE;
}

// ---------------------------------------------------------------- ecualizador

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} biquad_t;

static biquad_t s_bass, s_treble;
static float s_eq_pregain = 1.0f;
static volatile int s_eq_bass_db, s_eq_treble_db;
static volatile bool s_eq_dirty = true;

// Filtros de estante del "Audio EQ Cookbook" (R. Bristow-Johnson), pendiente S = 1.
static void shelf(biquad_t *f, bool low, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / AUDIO_RATE;
    float c = cosf(w0), alpha = sinf(w0) / 2.0f * sqrtf(2.0f), sa = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (low) {
        b0 = A * ((A + 1) - (A - 1) * c + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * c + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - sa;
    }
    *f = (biquad_t){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0};
}

static inline float biquad_run(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

void audio_set_eq(int bass_db, int treble_db)
{
    s_eq_bass_db = bass_db;
    s_eq_treble_db = treble_db;
    s_eq_dirty = true;
}

static void eq_process(int16_t *x, size_t n)
{
    if (s_eq_dirty) {  // se recalcula en la tarea de reproducción: sin carreras
        shelf(&s_bass, true, 250, s_eq_bass_db);
        shelf(&s_treble, false, 3000, s_eq_treble_db);
        int boost = s_eq_bass_db > s_eq_treble_db ? s_eq_bass_db : s_eq_treble_db;
        s_eq_pregain = boost > 0 ? powf(10.0f, -boost / 20.0f) : 1.0f;  // margen contra saturación
        s_eq_dirty = false;
    }
    if (s_eq_bass_db == 0 && s_eq_treble_db == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float y = biquad_run(&s_treble, biquad_run(&s_bass, x[i] * s_eq_pregain));
        x[i] = (int16_t)(y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y));
    }
}

// ---------------------------------------------------------------- tareas

static void playback_task(void *arg)
{
    static int16_t buf[AUDIO_FRAME_SAMPLES];
    const size_t frame = sizeof(buf);
    int muted = -1;  // estado aplicado al DAC; -1 obliga a aplicarlo la primera vez
    while (true) {
        size_t used = ring_used();
        if (!s_playing && used > 0) {
            int64_t waited = (esp_timer_get_time() - s_first_byte_us) / 1000;
            if (used >= PREBUFFER_MS * AUDIO_RATE / 1000 * 2 || waited >= PREBUFFER_WAIT_MS) {
                s_playing = true;
            }
        }
        size_t got = 0;
        if (s_playing) {
            got = ring_pop((uint8_t *)buf, frame);
            if (got < frame) {
                s_playing = false;
                s_drains++;
            }
            s_last_play_us = esp_timer_get_time();
        }
        // Se quita el silencio antes de escribir la primera trama con audio; el DMA todavía
        // tiene ceros en cola, así que el cambio no se oye como chasquido.
        audio_mute_t mode = s_mute_mode;
        int want = mode == AUDIO_MUTE_ON || (mode == AUDIO_MUTE_AUTO && !audio_playing());
        if (want != muted) {
            // El amplificador (PW_AMP) también se apaga: si no, amplifica el ruido que la radio
            // Wi-Fi mete en la alimentación aunque el DAC esté en silencio (así lo hace xiaozhi).
            esp_codec_dev_set_out_mute(s_codec, want);
            audio_set_amp(!want);
            muted = want;
        }
        memset((uint8_t *)buf + got, 0, frame - got);
        eq_process(buf, AUDIO_FRAME_SAMPLES);
        // Siempre escribir (silencio si no hay audio): el I2S marca el ritmo y no hay chasquidos.
        esp_codec_dev_write(s_codec, buf, frame);
    }
}

static void mic_task(void *arg)
{
    static int16_t buf[AUDIO_FRAME_SAMPLES];
    double acc = 0;
    int frames = 0;
    while (true) {
        if (esp_codec_dev_read(s_codec, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            continue;
        }
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            acc += (double)buf[i] * buf[i];
        }
        if (++frames == 50) {  // 1 s
            double rms = sqrt(acc / (50.0 * AUDIO_FRAME_SAMPLES));
            s_mic_dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -120;
            acc = 0;
            frames = 0;
        }
        if (s_mic_cb) {
            s_mic_cb(buf, AUDIO_FRAME_SAMPLES);
        }
    }
}

void audio_start(audio_mic_cb_t cb)
{
    s_mic_cb = cb;
    xTaskCreatePinnedToCore(playback_task, "playback", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 9, NULL, 1);
}
