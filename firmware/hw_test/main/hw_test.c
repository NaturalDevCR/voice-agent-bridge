// hw_test — Hito 1: prueba de hardware AtomS3R + Atomic Audio-3.5 Base.
//
// Consola interactiva por USB (idf.py monitor). Comandos: help, scan, amp, vol, gain,
// tone, rec, play, meter, duplex, info. Botón del Atom: graba 5 s y reproduce.
//
// Pinout y direcciones según docs/01 §1.1 (I2S 6/8/5/7 sin MCLK, I2C audio 38/39,
// ES8311 0x18, PI4IOE 0x43, I2C interno 45/0). La inicialización del ES8311 sigue la
// placa atoms3r-echo-base de xiaozhi-esp32 (MIT).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_chip_info.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hw_test";

#define SAMPLE_RATE      16000
#define FRAME_SAMPLES    320  // 20 ms
#define REC_MAX_SECONDS  10

#define PIN_I2S_WS    GPIO_NUM_6
#define PIN_I2S_BCLK  GPIO_NUM_8
#define PIN_I2S_DOUT  GPIO_NUM_5
#define PIN_I2S_DIN   GPIO_NUM_7
#define PIN_I2C_SDA   GPIO_NUM_38
#define PIN_I2C_SCL   GPIO_NUM_39
#define PIN_I2C_INT_SDA GPIO_NUM_45
#define PIN_I2C_INT_SCL GPIO_NUM_0
#define PIN_BUTTON    GPIO_NUM_41

#define ES8311_ADDR   0x18
#define PI4IOE_ADDR   0x43
#define LP5562_ADDR   0x30

#define PI4IOE_REG_ID      0x01
#define PI4IOE_REG_IO_DIR  0x03
#define PI4IOE_REG_IO_OUT  0x05
#define PI4IOE_REG_IO_HIZ  0x07
#define PI4IOE_REG_PULLUP  0x0D

static i2c_master_bus_handle_t s_bus;      // audio (externo)
static i2c_master_bus_handle_t s_bus_int;  // interno del AtomS3R
static i2c_master_dev_handle_t s_pi4ioe;
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_codec;
static SemaphoreHandle_t s_audio_lock;

static int s_volume = 40;      // 0–100, conservador para audífonos
static float s_gain_db = 30;   // ganancia del micrófono
static int16_t *s_rec;         // búfer de grabación en PSRAM
static size_t s_rec_samples;   // muestras válidas en s_rec

// ---------------------------------------------------------------- I2C

static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = PIN_I2C_INT_SDA;
    cfg.scl_io_num = PIN_I2C_INT_SCL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus_int));
}

static int scan_bus(i2c_master_bus_handle_t bus, const char *name)
{
    int found = 0;
    printf("Bus %s:", name);
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            printf(" 0x%02x", addr);
            found++;
        }
    }
    printf(found ? "\n" : " (nada)\n");
    return found;
}

static bool probe(i2c_master_bus_handle_t bus, uint8_t addr)
{
    return i2c_master_probe(bus, addr, 50) == ESP_OK;
}

static void scan_report(void)
{
    scan_bus(s_bus, "audio  (SDA 38 / SCL 39)");
    scan_bus(s_bus_int, "interno (SDA 45 / SCL 0)");
    bool es = probe(s_bus, ES8311_ADDR), io = probe(s_bus, PI4IOE_ADDR), lp = probe(s_bus_int, LP5562_ADDR);
    printf("  ES8311 0x18: %s\n  PI4IOE 0x43: %s\n  LP5562 0x30: %s\n",
           es ? "OK" : "NO ENCONTRADO", io ? "OK" : "NO ENCONTRADO", lp ? "OK" : "NO ENCONTRADO");
}

static esp_err_t pi4ioe_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pi4ioe, buf, 2, 100);
}

static esp_err_t pi4ioe_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pi4ioe, &reg, 1, val, 1, 100);
}

static void pi4ioe_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IOE_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_pi4ioe));
    uint8_t id = 0;
    if (pi4ioe_read(PI4IOE_REG_ID, &id) != ESP_OK) {
        ESP_LOGE(TAG, "PI4IOE no responde");
        return;
    }
    ESP_LOGI(TAG, "PI4IOE id/ctrl=0x%02x", id);
    // Igual que xiaozhi (0x6E) más P0 (PW_AMP) como salida.
    pi4ioe_write(PI4IOE_REG_IO_HIZ, 0x00);
    pi4ioe_write(PI4IOE_REG_PULLUP, 0xFF);
    pi4ioe_write(PI4IOE_REG_IO_DIR, 0x6F);
    pi4ioe_write(PI4IOE_REG_IO_OUT, 0xFF);
}

static void amp_set(bool on)
{
    uint8_t out = 0xFF;
    pi4ioe_read(PI4IOE_REG_IO_OUT, &out);
    out = on ? (out | 0x01) : (out & ~0x01);
    pi4ioe_write(PI4IOE_REG_IO_OUT, out);
    ESP_LOGI(TAG, "PW_AMP (P0) = %s", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------- Audio

static void audio_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, &s_rx));

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {.port = I2C_NUM_1, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new falló");
        return;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_codec, s_gain_db));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_codec, s_volume));
    ESP_LOGI(TAG, "ES8311 listo: %d Hz, mono, vol %d, ganancia mic %.0f dB", SAMPLE_RATE, s_volume, s_gain_db);
}

typedef struct {
    double sum_sq;
    double sum;
    int peak;
    size_t n;
} stats_t;

static void stats_add(stats_t *s, const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = x[i];
        s->sum_sq += (double)v * v;
        s->sum += v;
        if (abs(v) > s->peak) {
            s->peak = abs(v);
        }
    }
    s->n += n;
}

static double dbfs(double v)
{
    return v <= 0 ? -120.0 : 20.0 * log10(v / 32768.0);
}

static void stats_print(const char *label, const stats_t *s)
{
    if (!s->n) {
        return;
    }
    double rms = sqrt(s->sum_sq / s->n);
    printf("%s: RMS %.1f dBFS (%.0f) · pico %.1f dBFS · DC %.0f · %u muestras\n",
           label, dbfs(rms), rms, dbfs(s->peak), s->sum / s->n, (unsigned)s->n);
}

static int write_tone(float hz, float level_dbfs, float seconds)
{
    int16_t buf[FRAME_SAMPLES];
    float amp = 32767.0f * powf(10.0f, level_dbfs / 20.0f);
    float phase = 0, step = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    int frames = (int)(seconds * SAMPLE_RATE / FRAME_SAMPLES), errors = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            buf[i] = (int16_t)(amp * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        if (esp_codec_dev_write(s_codec, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            errors++;
        }
    }
    // Silencio para vaciar el DMA sin chasquido.
    memset(buf, 0, sizeof(buf));
    for (int f = 0; f < 5; f++) {
        esp_codec_dev_write(s_codec, buf, sizeof(buf));
    }
    return errors;
}

static int record(float seconds, bool meter)
{
    size_t want = (size_t)(seconds * SAMPLE_RATE);
    size_t cap = (size_t)REC_MAX_SECONDS * SAMPLE_RATE;
    if (want > cap) {
        want = cap;
    }
    stats_t total = {0}, window = {0};
    int errors = 0;
    int64_t t0 = esp_timer_get_time();
    s_rec_samples = 0;
    while (s_rec_samples < want) {
        size_t n = want - s_rec_samples < FRAME_SAMPLES ? want - s_rec_samples : FRAME_SAMPLES;
        int16_t *dst = s_rec + s_rec_samples;
        if (esp_codec_dev_read(s_codec, dst, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            errors++;
            continue;
        }
        stats_add(&total, dst, n);
        stats_add(&window, dst, n);
        s_rec_samples += n;
        if (meter && window.n >= SAMPLE_RATE / 5) {  // cada 200 ms
            double rms = sqrt(window.sum_sq / window.n);
            int bars = (int)((dbfs(rms) + 90) / 2);
            printf("%6.1f dBFS |%.*s\n", dbfs(rms), bars < 0 ? 0 : (bars > 45 ? 45 : bars),
                   "#############################################");
            memset(&window, 0, sizeof(window));
        }
    }
    double secs = (esp_timer_get_time() - t0) / 1e6;
    stats_print("Micrófono", &total);
    printf("Duración real %.3f s para %.3f s de audio (tasa efectiva %.0f Hz) · errores %d\n",
           secs, (double)s_rec_samples / SAMPLE_RATE, s_rec_samples / secs, errors);
    return errors;
}

static void play_recording(void)
{
    size_t off = 0;
    while (off < s_rec_samples) {
        size_t n = s_rec_samples - off < FRAME_SAMPLES ? s_rec_samples - off : FRAME_SAMPLES;
        esp_codec_dev_write(s_codec, s_rec + off, n * sizeof(int16_t));
        off += n;
    }
    int16_t zero[FRAME_SAMPLES] = {0};
    for (int f = 0; f < 5; f++) {
        esp_codec_dev_write(s_codec, zero, sizeof(zero));
    }
}

// ---------------------------------------------------------------- Duplex

typedef struct {
    float hz, dbfs, seconds;
    int errors;
    SemaphoreHandle_t done;
} tone_job_t;

static void tone_task(void *arg)
{
    tone_job_t *job = arg;
    job->errors = write_tone(job->hz, job->dbfs, job->seconds);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static void duplex_test(float seconds, float level)
{
    printf("Silencio de referencia 2 s…\n");
    record(2, false);
    stats_t quiet = {0};
    stats_add(&quiet, s_rec, s_rec_samples);

    printf("Tono 1 kHz %.0f dBFS durante %.0f s mientras se graba…\n", level, seconds);
    tone_job_t job = {.hz = 1000, .dbfs = level, .seconds = seconds, .done = xSemaphoreCreateBinary()};
    xTaskCreatePinnedToCore(tone_task, "tone", 4096, &job, 6, NULL, 1);
    int rec_err = record(seconds, false);
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);

    stats_t loud = {0};
    // Descartar los primeros 200 ms (arranque del tono).
    size_t skip = SAMPLE_RATE / 5 < s_rec_samples ? SAMPLE_RATE / 5 : 0;
    stats_add(&loud, s_rec + skip, s_rec_samples - skip);
    stats_print("Reposo", &quiet);
    stats_print("Con tono", &loud);
    double q = sqrt(quiet.sum_sq / quiet.n), l = sqrt(loud.sum_sq / loud.n);
    printf("Acoplamiento de eco: %.1f dB sobre el reposo · errores I2S tx %d rx %d\n",
           dbfs(l) - dbfs(q), job.errors, rec_err);
}

// ---------------------------------------------------------------- Consola

static float argf(int argc, char **argv, int i, float def)
{
    return argc > i ? strtof(argv[i], NULL) : def;
}

#define LOCKED(...)                                                        \
    do {                                                                   \
        if (xSemaphoreTake(s_audio_lock, pdMS_TO_TICKS(100)) != pdTRUE) {  \
            printf("Audio ocupado\n");                                     \
            return 1;                                                      \
        }                                                                  \
        __VA_ARGS__;                                                       \
        xSemaphoreGive(s_audio_lock);                                      \
    } while (0)

static int cmd_scan(int argc, char **argv)
{
    scan_report();
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("ESP32-S3 rev %d, %d núcleos · PSRAM %u KB · heap interno libre %u · PSRAM libre %u\n",
           chip.revision, chip.cores, (unsigned)(esp_psram_get_size() / 1024),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Volumen %d · ganancia mic %.0f dB · %d Hz\n", s_volume, s_gain_db, SAMPLE_RATE);
    return 0;
}

static int cmd_amp(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: amp on|off\n");
        return 1;
    }
    amp_set(strcmp(argv[1], "on") == 0);
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc < 2) {
        printf("Volumen %d\n", s_volume);
        return 0;
    }
    s_volume = atoi(argv[1]);
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    printf("Volumen %d\n", s_volume);
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    if (argc < 2) {
        printf("Ganancia mic %.0f dB\n", s_gain_db);
        return 0;
    }
    s_gain_db = strtof(argv[1], NULL);
    esp_codec_dev_set_in_gain(s_codec, s_gain_db);
    printf("Ganancia mic %.0f dB\n", s_gain_db);
    return 0;
}

static int cmd_tone(int argc, char **argv)
{
    float hz = argf(argc, argv, 1, 1000), level = argf(argc, argv, 2, -20), secs = argf(argc, argv, 3, 3);
    printf("Tono %.0f Hz, %.0f dBFS, %.1f s, volumen %d\n", hz, level, secs, s_volume);
    LOCKED(int e = write_tone(hz, level, secs); printf("Listo · errores %d\n", e));
    return 0;
}

static int cmd_rec(int argc, char **argv)
{
    float secs = argf(argc, argv, 1, 5);
    printf("Grabando %.1f s…\n", secs);
    LOCKED(record(secs, false));
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    printf("Reproduciendo %.1f s\n", (double)s_rec_samples / SAMPLE_RATE);
    LOCKED(play_recording());
    return 0;
}

static int cmd_meter(int argc, char **argv)
{
    float secs = argf(argc, argv, 1, 10);
    LOCKED(record(secs, true));
    return 0;
}

static int cmd_duplex(int argc, char **argv)
{
    float secs = argf(argc, argv, 1, 5), level = argf(argc, argv, 2, -20);
    LOCKED(duplex_test(secs, level));
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hw> ";
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "scan", .help = "Escanea ambos buses I2C", .func = cmd_scan},
        {.command = "info", .help = "Chip, memoria y ajustes de audio", .func = cmd_info},
        {.command = "amp", .help = "amp on|off — PW_AMP (P0 del PI4IOE)", .func = cmd_amp},
        {.command = "vol", .help = "vol [0-100] — volumen de salida del ES8311", .func = cmd_vol},
        {.command = "gain", .help = "gain [dB] — ganancia del micrófono", .func = cmd_gain},
        {.command = "tone", .help = "tone [Hz=1000] [dBFS=-20] [s=3]", .func = cmd_tone},
        {.command = "rec", .help = "rec [s=5] — graba en PSRAM y muestra RMS", .func = cmd_rec},
        {.command = "play", .help = "Reproduce la última grabación", .func = cmd_play},
        {.command = "meter", .help = "meter [s=10] — medidor de nivel cada 200 ms", .func = cmd_meter},
        {.command = "duplex", .help = "duplex [s=5] [dBFS=-20] — tono + grabación, mide eco", .func = cmd_duplex},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

// ---------------------------------------------------------------- Botón

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    while (true) {
        if (gpio_get_level(PIN_BUTTON) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(PIN_BUTTON) == 0 && xSemaphoreTake(s_audio_lock, 0) == pdTRUE) {
                // Un pitido agudo: empieza a grabar. Dos graves: terminó, reproduce.
                printf("\n[botón] Grabando 5 s…\n");
                write_tone(1320, -20, 0.15f);
                record(5, false);
                write_tone(660, -20, 0.12f);
                write_tone(660, -20, 0.12f);
                printf("[botón] Reproduciendo\n");
                play_recording();
                xSemaphoreGive(s_audio_lock);
            }
            while (gpio_get_level(PIN_BUTTON) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "hw_test arrancando");
    s_audio_lock = xSemaphoreCreateMutex();
    s_rec = heap_caps_malloc((size_t)REC_MAX_SECONDS * SAMPLE_RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_rec) {
        ESP_LOGE(TAG, "Sin PSRAM para el búfer de grabación");
    }

    i2c_init();
    scan_report();
    if (probe(s_bus, PI4IOE_ADDR)) {
        pi4ioe_init();
    }
    if (probe(s_bus, ES8311_ADDR)) {
        audio_init();
    } else {
        ESP_LOGE(TAG, "Sin ES8311: ¿está el Atom bien asentado en la base?");
    }

    if (s_codec) {
        xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
    }
    console_start();
}
