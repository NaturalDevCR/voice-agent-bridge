#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *NS = "terminal";

// Valores medidos en el Hito 1 (docs/03): jack con audífonos necesita volumen alto,
// 30 dB de ganancia deja la voz en −20…−27 dBFS. El jack suena sin graves: EQ de partida.
term_config_t g_cfg = {.name = "Asistente de voz", .volume = 100, .gain_db = 30, .tx_power = 20, .eq_bass = 6, .eq_treble = -4};

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
    }
}

void config_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "sin configuración guardada");
        return;
    }
    load_str(h, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
    load_str(h, "pass", g_cfg.pass, sizeof(g_cfg.pass));
    load_str(h, "url", g_cfg.url, sizeof(g_cfg.url));
    load_str(h, "token", g_cfg.token, sizeof(g_cfg.token));
    char name[sizeof(g_cfg.name)];
    load_str(h, "name", name, sizeof(name));
    if (name[0]) {
        strlcpy(g_cfg.name, name, sizeof(g_cfg.name));
    }
    int32_t v;
    if (nvs_get_i32(h, "volume", &v) == ESP_OK) {
        g_cfg.volume = v;
    }
    if (nvs_get_i32(h, "gain", &v) == ESP_OK) {
        g_cfg.gain_db = v;
    }
    if (nvs_get_i32(h, "tx_power", &v) == ESP_OK) {
        g_cfg.tx_power = v;
    }
    if (nvs_get_i32(h, "eq_bass", &v) == ESP_OK) {
        g_cfg.eq_bass = v;
    }
    if (nvs_get_i32(h, "eq_treble", &v) == ESP_OK) {
        g_cfg.eq_treble = v;
    }
    nvs_close(h);
}

bool config_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = ESP_OK;
    err |= nvs_set_str(h, "ssid", g_cfg.ssid);
    err |= nvs_set_str(h, "pass", g_cfg.pass);
    err |= nvs_set_str(h, "url", g_cfg.url);
    err |= nvs_set_str(h, "token", g_cfg.token);
    err |= nvs_set_str(h, "name", g_cfg.name);
    err |= nvs_set_i32(h, "volume", g_cfg.volume);
    err |= nvs_set_i32(h, "gain", g_cfg.gain_db);
    err |= nvs_set_i32(h, "tx_power", g_cfg.tx_power);
    err |= nvs_set_i32(h, "eq_bass", g_cfg.eq_bass);
    err |= nvs_set_i32(h, "eq_treble", g_cfg.eq_treble);
    err |= nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool config_ready(void)
{
    return g_cfg.ssid[0] && g_cfg.url[0] && g_cfg.token[0];
}

void config_print(void)
{
    printf("wifi   : %s (clave %s)\n", g_cfg.ssid[0] ? g_cfg.ssid : "—", g_cfg.pass[0] ? "guardada" : "vacía");
    printf("url    : %s\n", g_cfg.url[0] ? g_cfg.url : "—");
    printf("token  : %s\n", g_cfg.token[0] ? "guardado" : "—");
    printf("nombre : %s\n", g_cfg.name);
    printf("radio  : %d dBm máx.\n", g_cfg.tx_power);
    printf("volumen: %d · ganancia mic: %d dB · EQ graves %+d dB, agudos %+d dB\n", g_cfg.volume, g_cfg.gain_db,
           g_cfg.eq_bass, g_cfg.eq_treble);
}
