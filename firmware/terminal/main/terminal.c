// Terminal de voz — AtomS3R + Atomic Audio-3.5 Base ⇄ Voice Agent Bridge (protocolo v0).
//
// Sólo mueve audio y botones: el puente decide sesiones, habla con ElevenLabs y con
// Home Assistant (docs/01 §3). Protocolo en bridge/voice_agent_bridge/terminals/websocket.py:
//   subida  binario PCM s16le 16 kHz mono (20 ms) mientras mic=on;
//           texto {"t":"hello"} {"t":"button","ev":"short"} {"t":"stats",...}
//   bajada  binario PCM s16le 16 kHz mono; texto {"t":"mic"} {"t":"flush"} {"t":"state"} {"t":"volume"}
//
// Wi-Fi, URL del puente y token se guardan en NVS desde la consola USB (comando `help`).

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "terminal";

#define PIN_BUTTON     GPIO_NUM_41
#define STATS_PERIOD_S 5
#define UI_PERIOD_MS   500
#define MIC_QUEUE_LEN  50  // 1 s de margen ante pausas de Wi-Fi
// Tramas de 20 ms por envío. Cada envío es una ráfaga de la radio, y cada ráfaga se oye como
// ruido en el audio: 5 tramas = 100 ms por paquete, 10 envíos/s en vez de 50.
#define MIC_BATCH      5
// Tope de un envío. Si esp_websocket_client no logra escribir nada en ese plazo, da la
// conexión por perdida: debe cubrir las pausas normales del Wi-Fi (100–500 ms).
#define SEND_TIMEOUT_MS 1500

typedef struct {
    int16_t pcm[AUDIO_FRAME_SAMPLES];
} mic_frame_t;

static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_mic_q;
static char s_headers[128];
static char s_url[200];
static char s_terminal_id[24];
static char s_state[16] = "—";
static char s_text[256];
static size_t s_text_len;
static int s_rx_op;
static volatile bool s_ws_connected, s_mic_on, s_wifi_up;
static volatile uint32_t s_mic_drops, s_wifi_drops, s_bridge_drops;
static volatile bool s_radio_off, s_scanning;  // radio apagada a propósito / escaneo manual  // radio apagada a propósito (comando radio)
static volatile int64_t s_online_since_us;  // inicio de la conexión actual al puente

// ---------------------------------------------------------------- JSON mínimo
// Los mensajes del puente son planos y cortos (json.dumps con espacios tras ':' y ',').

static const char *json_value(const char *msg, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(msg, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool json_str(const char *msg, const char *key, char *out, size_t size)
{
    const char *p = json_value(msg, key);
    if (!p || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

// ---------------------------------------------------------------- WebSocket

static void ws_send_text(const char *text)
{
    if (s_ws_connected) {
        esp_websocket_client_send_text(s_ws, text, strlen(text), pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    }
}

static void send_hello(void)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"t\":\"hello\",\"fw\":\"%s\",\"rate\":%d,\"proto\":0,\"id\":\"%s\"}",
             esp_app_get_description()->version, AUDIO_RATE, s_terminal_id);
    ws_send_text(msg);
}

static void handle_text(const char *msg)
{
    char t[16];
    if (!json_str(msg, "t", t, sizeof(t))) {
        return;
    }
    if (strcmp(t, "mic") == 0) {
        const char *v = json_value(msg, "on");
        s_mic_on = v && strncmp(v, "true", 4) == 0;
        ESP_LOGI(TAG, "micrófono %s", s_mic_on ? "ON" : "OFF");
    } else if (strcmp(t, "flush") == 0) {
        audio_play_flush();
        ESP_LOGI(TAG, "flush");
    } else if (strcmp(t, "state") == 0) {
        json_str(msg, "s", s_state, sizeof(s_state));
        ESP_LOGI(TAG, "estado: %s", s_state);
    } else if (strcmp(t, "volume") == 0) {
        const char *v = json_value(msg, "v");
        if (v) {
            g_cfg.volume = (int)(strtof(v, NULL) * 100);
            audio_set_volume(g_cfg.volume);
            ESP_LOGI(TAG, "volumen %d", g_cfg.volume);
        }
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "conectado al puente");
        s_ws_connected = true;
        s_online_since_us = esp_timer_get_time();
        send_hello();
        audio_beep(1320, 80, 1);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_ws_connected) {
            ESP_LOGW(TAG, "desconectado del puente");
            s_bridge_drops++;
        }
        s_ws_connected = false;
        s_mic_on = false;
        audio_play_flush();
        break;
    case WEBSOCKET_EVENT_DATA:
        // Un mensaje grande llega en trozos (payload_offset); el op_code vale para todos.
        if (d->payload_offset == 0 && d->op_code != 0x0) {
            s_rx_op = d->op_code;
            s_text_len = 0;
        }
        if (s_rx_op == 0x2) {
            audio_play_push((const uint8_t *)d->data_ptr, d->data_len);
        } else if (s_rx_op == 0x1) {
            size_t n = d->data_len;
            if (s_text_len + n >= sizeof(s_text)) {
                n = sizeof(s_text) - 1 - s_text_len;
            }
            memcpy(s_text + s_text_len, d->data_ptr, n);
            s_text_len += n;
            if (d->payload_offset + d->data_len >= d->payload_len) {
                s_text[s_text_len] = '\0';
                handle_text(s_text);
                s_text_len = 0;
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "error de WebSocket (¿URL, token o puente apagado?)");
        break;
    default:
        break;
    }
}

static void ws_start(void)
{
    if (s_ws) {
        return;
    }
    // Sin ?id= en la URL, el puente vería "terminal": usar el id derivado de la MAC.
    snprintf(s_url, sizeof(s_url), "%s%sid=%s", g_cfg.url, strchr(g_cfg.url, '?') ? "&" : "?", s_terminal_id);
    if (strstr(g_cfg.url, "id=")) {
        snprintf(s_url, sizeof(s_url), "%s", g_cfg.url);
    }
    snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", g_cfg.token);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = s_headers,
        .buffer_size = 4096,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
        .task_stack = 6144,
        .task_prio = 8,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    ESP_LOGI(TAG, "conectando a %s", s_url);
    esp_websocket_client_start(s_ws);
}

// Tarea del micrófono → cola → esta tarea: un envío lento nunca frena la lectura del I2S.
static void mic_cb(const int16_t *pcm, size_t samples)
{
    if (!s_ws_connected || !s_mic_on) {
        return;
    }
    mic_frame_t frame;
    if (audio_playing()) {
        memset(frame.pcm, 0, sizeof(frame.pcm));  // half-duplex E0: no oírse a sí mismo
    } else {
        memcpy(frame.pcm, pcm, sizeof(frame.pcm));
    }
    if (xQueueSend(s_mic_q, &frame, 0) != pdTRUE) {
        // Cola llena (red lenta): descartar la trama más vieja; la voz reciente importa más.
        mic_frame_t old;
        xQueueReceive(s_mic_q, &old, 0);
        xQueueSend(s_mic_q, &frame, 0);
        s_mic_drops++;
    }
}

static void sender_task(void *arg)
{
    static int16_t batch[MIC_BATCH * AUDIO_FRAME_SAMPLES];
    mic_frame_t frame;
    int n = 0;
    while (true) {
        if (xQueueReceive(s_mic_q, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_ws_connected || !s_mic_on) {
            n = 0;
            continue;
        }
        memcpy(batch + n * AUDIO_FRAME_SAMPLES, frame.pcm, sizeof(frame.pcm));
        if (++n < MIC_BATCH) {
            continue;
        }
        n = 0;
        if (esp_websocket_client_send_bin(s_ws, (const char *)batch, sizeof(batch), pdMS_TO_TICKS(SEND_TIMEOUT_MS)) <
            0) {
            s_mic_drops += MIC_BATCH;
        }
    }
}

static void stats_task(void *arg)
{
    char msg[256];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_S * 1000));
        if (!s_ws_connected) {
            continue;
        }
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        audio_stats_t st;
        audio_get_stats(&st);
        snprintf(msg, sizeof(msg),
                 "{\"t\":\"stats\",\"rssi\":%d,\"heap\":%u,\"psram\":%u,\"underruns\":%u,\"overruns\":%u,"
                 "\"mic_drops\":%u,\"mic_rms\":%.1f,\"uptime\":%lld}",
                 ap.rssi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)st.drains,
                 (unsigned)st.overruns, (unsigned)s_mic_drops, st.mic_dbfs, esp_timer_get_time() / 1000000);
        ws_send_text(msg);
    }
}

static void ui_task(void *arg)
{
    display_info_t info = {0};
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
        wifi_ap_record_t ap = {0};
        bool assoc = s_wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        info.link = !config_ready()   ? DISPLAY_NO_CONFIG
                    : !s_wifi_up      ? DISPLAY_NO_WIFI
                    : !s_ws_connected ? DISPLAY_NO_BRIDGE
                                      : DISPLAY_ONLINE;
        info.state = s_state;
        info.playing = audio_playing();
        info.rssi = assoc ? ap.rssi : 0;
        info.channel = assoc ? ap.primary : 0;
        info.wifi_drops = s_wifi_drops;
        info.bridge_drops = s_bridge_drops;
        info.online_s = s_ws_connected ? (esp_timer_get_time() - s_online_since_us) / 1000000 : 0;
        display_show(&info);
    }
}

// ---------------------------------------------------------------- Wi-Fi

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        static uint32_t fails;
        if (s_wifi_up) {
            ESP_LOGW(TAG, "Wi-Fi perdido (motivo %d), reintentando", e->reason);
            s_wifi_drops++;
            fails = 0;
        } else if (fails++ % 10 == 0) {  // sin inundar el log mientras reintenta
            ESP_LOGW(TAG, "no conecta a \"%s\" (motivo %d, intento %lu)", g_cfg.ssid, e->reason,
                     (unsigned long)fails);
        }
        s_wifi_up = false;
        if (!s_radio_off && !s_scanning) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        ESP_LOGI(TAG, "Wi-Fi OK, IP " IPSTR " · AP " MACSTR " · canal %d · RSSI %d dBm", IP2STR(&e->ip_info.ip),
                 MAC2STR(ap.bssid), ap.primary, ap.rssi);
        s_wifi_up = true;
        ws_start();
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_cfg.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // Por defecto ESP-IDF hace WIFI_FAST_SCAN: se queda con el primer AP que tenga el SSID,
    // aunque esté lejos. Con varios APs (UniFi) hay que escanear todo y elegir por señal.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.rm_enabled = 1;   // 802.11k/v: el AP puede sugerir otro mejor (BSS transition)
    wc.sta.btm_enabled = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // latencia de audio estable
    esp_wifi_set_max_tx_power((int8_t)(g_cfg.tx_power * 4));  // unidades de 0,25 dBm
    ESP_LOGI(TAG, "conectando a Wi-Fi \"%s\"", g_cfg.ssid);
}

// Escanea la red configurada. Devuelve cuántos APs se encontraron (máximo `max`).
// Escanea la red configurada (o todas, con ssid NULL). Devuelve cuántos APs (máximo `max`).
static uint16_t wifi_scan_ssid(const char *ssid, wifi_ap_record_t *recs, uint16_t max)
{
    wifi_scan_config_t sc = {.ssid = (uint8_t *)ssid};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return 0;
    }
    return n;
}

static uint16_t wifi_scan(wifi_ap_record_t *recs, uint16_t max)
{
    return wifi_scan_ssid(g_cfg.ssid, recs, max);
}

// Roaming propio, para APs sin 802.11v: con señal débil y sin conversación en curso, si hay
// otro AP de la misma red bastante mejor, se reconecta (el escaneo completo elige el mejor).
#define ROAM_PERIOD_S  30
#define ROAM_RSSI      -70
#define ROAM_MARGIN_DB 8

static void roam_task(void *arg)
{
    static wifi_ap_record_t recs[16];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ROAM_PERIOD_S * 1000));
        wifi_ap_record_t cur;
        if (!s_wifi_up || s_mic_on || audio_playing() || esp_wifi_sta_get_ap_info(&cur) != ESP_OK ||
            cur.rssi >= ROAM_RSSI) {
            continue;
        }
        uint16_t n = wifi_scan(recs, sizeof(recs) / sizeof(recs[0]));
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (memcmp(recs[i].bssid, cur.bssid, 6) != 0 && (best < 0 || recs[i].rssi > recs[best].rssi)) {
                best = i;
            }
        }
        if (best >= 0 && recs[best].rssi >= cur.rssi + ROAM_MARGIN_DB && !s_mic_on && !audio_playing()) {
            ESP_LOGW(TAG, "roaming: AP actual %d dBm, " MACSTR " tiene %d dBm", cur.rssi, MAC2STR(recs[best].bssid),
                     recs[best].rssi);
            esp_wifi_disconnect();  // el evento de desconexión reconecta al más fuerte
        }
    }
}

// ---------------------------------------------------------------- botón

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
            if (gpio_get_level(PIN_BUTTON) == 0) {
                if (s_ws_connected) {
                    ESP_LOGI(TAG, "botón → puente");
                    ws_send_text("{\"t\":\"button\",\"ev\":\"short\"}");
                } else {
                    ESP_LOGW(TAG, "botón sin conexión al puente");
                    audio_beep(330, 120, 2);
                }
                while (gpio_get_level(PIN_BUTTON) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------- consola

static int need_args(int argc, int n, const char *usage)
{
    if (argc < n) {
        printf("uso: %s\n", usage);
        return 1;
    }
    return 0;
}

static void saved(bool restart)
{
    printf(config_save() ? "Guardado.%s\n" : "ERROR al guardar%s\n", restart ? " Aplica con: reboot" : "");
}

static int cmd_wifi(int argc, char **argv)
{
    if (need_args(argc, 2, "wifi <ssid> [clave]   (comillas si hay espacios)")) {
        return 1;
    }
    if ((strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0) && argc == 2) {
        printf("¿Querías 'radio %s'? 'wifi' guarda la red. Para una red llamada así: wifi %s <clave>\n", argv[1],
               argv[1]);
        return 1;
    }
    strlcpy(g_cfg.ssid, argv[1], sizeof(g_cfg.ssid));
    strlcpy(g_cfg.pass, argc > 2 ? argv[2] : "", sizeof(g_cfg.pass));
    saved(true);
    return 0;
}

static int cmd_url(int argc, char **argv)
{
    if (need_args(argc, 2, "url ws://<IP de Home Assistant>:8765/v0/terminal")) {
        return 1;
    }
    strlcpy(g_cfg.url, argv[1], sizeof(g_cfg.url));
    saved(true);
    return 0;
}

static int cmd_token(int argc, char **argv)
{
    if (need_args(argc, 2, "token <token del puente>")) {
        return 1;
    }
    strlcpy(g_cfg.token, argv[1], sizeof(g_cfg.token));
    saved(true);
    return 0;
}

// Diagnóstico de interferencias en el audio: potencia de radio y retroiluminación.
static int cmd_txpower(int argc, char **argv)
{
    int8_t q;
    if (argc > 1) {
        int dbm = atoi(argv[1]);
        esp_err_t err = esp_wifi_set_max_tx_power((int8_t)(dbm * 4));  // unidades de 0,25 dBm
        if (err != ESP_OK) {
            printf("error: %s (rango 2–20 dBm, con Wi-Fi arrancado)\n", esp_err_to_name(err));
            return 1;
        }
        g_cfg.tx_power = dbm;
        saved(false);
    }
    if (esp_wifi_get_max_tx_power(&q) == ESP_OK) {
        printf("potencia Wi-Fi máxima: %.1f dBm\n", q / 4.0);
    }
    return 0;
}

// Apaga la radio por completo: si el ruido desaparece, viene del Wi-Fi y no del audio.
static int cmd_radio(int argc, char **argv)
{
    if (need_args(argc, 2, "radio off|on")) {
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        s_radio_off = true;
        esp_wifi_stop();
        printf("radio apagada (sin puente). Vuelve con: radio on\n");
    } else {
        // Reiniciar es lo más fiable para volver a levantar Wi-Fi, IP y WebSocket.
        printf("encendiendo la radio: reinicio\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    return 0;
}

static int cmd_backlight(int argc, char **argv)
{
    if (need_args(argc, 2, "backlight <0-100>   (0 = apagada, 100 = sin PWM)")) {
        return 1;
    }
    display_set_brightness(atoi(argv[1]));
    printf("retroiluminación %d %%\n", atoi(argv[1]));
    return 0;
}

static int cmd_name(int argc, char **argv)
{
    if (argc > 1) {
        strlcpy(g_cfg.name, argv[1], sizeof(g_cfg.name));
        saved(true);
    }
    printf("nombre: %s\n", g_cfg.name);
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc > 1) {
        g_cfg.volume = atoi(argv[1]);
        audio_set_volume(g_cfg.volume);
        saved(false);
    }
    printf("volumen %d\n", g_cfg.volume);
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    if (argc > 1) {
        g_cfg.gain_db = atoi(argv[1]);
        audio_set_gain(g_cfg.gain_db);
        saved(false);
    }
    printf("ganancia mic %d dB\n", g_cfg.gain_db);
    return 0;
}

static int cmd_eq(int argc, char **argv)
{
    if (argc > 2) {
        g_cfg.eq_bass = atoi(argv[1]);
        g_cfg.eq_treble = atoi(argv[2]);
        audio_set_eq(g_cfg.eq_bass, g_cfg.eq_treble);
        saved(false);
    } else if (argc > 1) {
        printf("uso: eq <graves dB> <agudos dB>   p. ej. eq 8 -6 · eq 0 0 = sin ecualizar\n");
        return 1;
    }
    printf("EQ graves %+d dB, agudos %+d dB\n", g_cfg.eq_bass, g_cfg.eq_treble);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    wifi_ap_record_t ap = {0};
    bool assoc = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    audio_stats_t st;
    audio_get_stats(&st);
    printf("fw %s · id %s\n", esp_app_get_description()->version, s_terminal_id);
    config_print();
    printf("Wi-Fi  : %s", s_wifi_up ? "conectado" : "sin conexión");
    if (assoc) {
        printf(" · AP " MACSTR " · RSSI %d dBm · canal %d", MAC2STR(ap.bssid), ap.rssi, ap.primary);
    }
    printf("\npuente : %s · estado %s · mic %s\n", s_ws_connected ? "conectado" : "sin conexión", s_state,
           s_mic_on ? "ON" : "OFF");
    printf("audio  : mic %.1f dBFS · pendiente %u ms · vaciados %u · desbordes %u · tramas perdidas %u\n",
           st.mic_dbfs, (unsigned)st.play_ms, (unsigned)st.drains, (unsigned)st.overruns, (unsigned)s_mic_drops);
    printf("memoria: interna %u · PSRAM %u · uptime %lld s\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), esp_timer_get_time() / 1000000);
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    static wifi_ap_record_t recs[16];
    wifi_ap_record_t cur = {0};
    bool assoc = esp_wifi_sta_get_ap_info(&cur) == ESP_OK;
    // Mientras reintenta conectar, ESP-IDF no deja escanear: pausar los reintentos.
    bool paused = !assoc && !s_radio_off;
    if (paused) {
        s_scanning = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    bool all = argc > 1 && strcmp(argv[1], "all") == 0;
    uint16_t n = wifi_scan_ssid(all ? NULL : g_cfg.ssid, recs, sizeof(recs) / sizeof(recs[0]));
    if (paused) {
        s_scanning = false;
        esp_wifi_connect();
    }
    if (n == 0) {
        printf("Sin APs de \"%s\" a la vista. Prueba: scan all\n", g_cfg.ssid);
        return 1;
    }
    if (all) {
        printf("Redes a la vista (las %u más fuertes):\n", (unsigned)n);
    } else {
        printf("APs de \"%s\":\n", g_cfg.ssid);
    }
    for (int i = 0; i < n; i++) {
        bool mine = assoc && memcmp(recs[i].bssid, cur.bssid, 6) == 0;
        printf("  %s " MACSTR "  canal %2d  %4d dBm  auth %d  %s\n", mine ? "→" : " ", MAC2STR(recs[i].bssid),
               recs[i].primary, recs[i].rssi, recs[i].authmode, (char *)recs[i].ssid);
    }
    return 0;
}

// Diagnóstico: reproduce un WAV PCM 16 kHz mono servido por HTTP, sin puente ni ElevenLabs.
// Separa la calidad de la salida analógica de la de la red.
static int cmd_playurl(int argc, char **argv)
{
    if (need_args(argc, 2, "playurl http://<ip>:<puerto>/archivo.wav")) {
        return 1;
    }
    esp_http_client_config_t cfg = {.url = argv[1], .timeout_ms = 5000};
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (esp_http_client_open(http, 0) != ESP_OK) {
        printf("No se pudo abrir %s\n", argv[1]);
        esp_http_client_cleanup(http);
        return 1;
    }
    esp_http_client_fetch_headers(http);
    static uint8_t buf[2048];
    int n, total = 0, skip = 44;  // cabecera WAV estándar
    printf("Reproduciendo… (volumen %d)\n", g_cfg.volume);
    while ((n = esp_http_client_read(http, (char *)buf, sizeof(buf))) > 0) {
        uint8_t *p = buf;
        if (skip) {
            int s = n < skip ? n : skip;
            p += s;
            n -= s;
            skip -= s;
        }
        while (n > 0) {
            audio_stats_t st;
            audio_get_stats(&st);
            if (st.play_ms > 2000) {  // no llenar el búfer de 30 s: ir al ritmo de la reproducción
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            size_t pushed = audio_play_push(p, n);
            p += pushed;
            n -= pushed;
            total += pushed;
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    printf("Enviado %.1f s de audio\n", total / 2.0 / AUDIO_RATE);
    return 0;
}

static int cmd_mute(int argc, char **argv)
{
    if (need_args(argc, 2, "mute auto|on|off")) {
        return 1;
    }
    if (strcmp(argv[1], "auto") == 0) {
        audio_set_mute(AUDIO_MUTE_AUTO);
        printf("DAC silenciado cuando no suena nada\n");
    } else {
        bool on = strcmp(argv[1], "on") == 0;
        audio_set_mute(on ? AUDIO_MUTE_ON : AUDIO_MUTE_OFF);
        printf("DAC %s siempre (vuelve a auto con reboot)\n", on ? "silenciado" : "activo");
    }
    return 0;
}

static int cmd_amp(int argc, char **argv)
{
    if (need_args(argc, 2, "amp on|off")) {
        return 1;
    }
    bool on = strcmp(argv[1], "on") == 0;
    audio_set_amp(on);
    printf("PW_AMP %s\n", on ? "ON" : "OFF");
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    int hz = argc > 1 ? atoi(argv[1]) : 1000;
    int ms = argc > 2 ? atoi(argv[2]) : 300;
    if (hz < 50 || hz > 7000 || ms < 10 || ms > 20000) {
        printf("uso: beep [Hz 50-7000] [ms 10-20000]   p. ej. beep 440 5000\n");
        return 1;
    }
    audio_beep(hz, ms, 1);  // se genera en el Atom: sirve con la radio apagada
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "terminal> ";
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl));
    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Estado de Wi-Fi, puente, audio y memoria", .func = cmd_status},
        {.command = "wifi", .help = "wifi <ssid> [clave] — guarda la red", .func = cmd_wifi},
        {.command = "scan", .help = "scan [all] — APs de la red con su señal (→ = el actual); all = todas las redes", .func = cmd_scan},
        {.command = "url", .help = "url ws://<IP HA>:8765/v0/terminal — guarda el puente", .func = cmd_url},
        {.command = "token", .help = "token <token> — guarda el token del puente", .func = cmd_token},
        {.command = "name", .help = "name \"<nombre>\" — nombre bajo el logo, sin tildes (se guarda)", .func = cmd_name},
        {.command = "vol", .help = "vol [0-100] — volumen de salida (se guarda)", .func = cmd_vol},
        {.command = "gain", .help = "gain [dB] — ganancia del micrófono (se guarda)", .func = cmd_gain},
        {.command = "eq", .help = "eq <graves dB> <agudos dB> — ecualizador de salida (se guarda)", .func = cmd_eq},
        {.command = "beep", .help = "beep [Hz] [ms] — tono de prueba generado en el Atom", .func = cmd_beep},
        {.command = "mute", .help = "mute auto|on|off — silencio del DAC (auto: cuando no suena nada)", .func = cmd_mute},
        {.command = "amp", .help = "amp on|off — amplificador interno PW_AMP (diagnóstico)", .func = cmd_amp},
        {.command = "playurl", .help = "playurl http://… .wav — reproduce un WAV 16 kHz (diagnóstico)", .func = cmd_playurl},
        {.command = "txpower", .help = "txpower [dBm] — potencia máxima de Wi-Fi, 2–20 (se guarda; menos = menos ruido)", .func = cmd_txpower},
        {.command = "radio", .help = "radio off|on — apaga el Wi-Fi hasta 'radio on' (reinicia) o reboot (diagnóstico de ruido)", .func = cmd_radio},
        {.command = "backlight", .help = "backlight <0-100> — brillo de pantalla (diagnóstico de ruido)", .func = cmd_backlight},
        {.command = "reboot", .help = "Reinicia el terminal", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

// ---------------------------------------------------------------- main

void app_main(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_terminal_id, sizeof(s_terminal_id), "atom-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "terminal %s, fw %s", s_terminal_id, esp_app_get_description()->version);

    config_load();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (display_init(g_cfg.name, esp_app_get_description()->version)) {
        xTaskCreate(ui_task, "ui", 4096, NULL, 2, NULL);
    }

    audio_set_eq(g_cfg.eq_bass, g_cfg.eq_treble);
    if (audio_init(g_cfg.volume, g_cfg.gain_db)) {
        s_mic_q = xQueueCreate(MIC_QUEUE_LEN, sizeof(mic_frame_t));
        audio_start(mic_cb);
        xTaskCreatePinnedToCore(sender_task, "mic-send", 4096, NULL, 8, NULL, 0);
    }
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, NULL);
    xTaskCreate(roam_task, "roam", 4096, NULL, 3, NULL);

    if (config_ready()) {
        wifi_start();
    } else {
        printf("\nFalta configuración. Usa: wifi, url, token y luego reboot (ver `help`).\n\n");
    }
    console_start();
}
