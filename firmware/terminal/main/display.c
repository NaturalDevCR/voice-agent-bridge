// Pantalla del AtomS3R (GC9107 o ST7735 según lote, 128×128, SPI) con LVGL.
// Pines, secuencia de inicio y retroiluminación por LP5562: placa "atoms3r-echo-base" de xiaozhi.
// Textos sin tildes: las fuentes Montserrat de LVGL sólo traen ASCII.

#include "display.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

#define PIN_LCD_MOSI GPIO_NUM_21
#define PIN_LCD_SCLK GPIO_NUM_15
#define PIN_LCD_CS   GPIO_NUM_14
#define PIN_LCD_DC   GPIO_NUM_42
#define PIN_LCD_RST  GPIO_NUM_48
#define PIN_INT_SDA  GPIO_NUM_45  // bus I2C interno: LP5562 (0x30) e IMU
#define PIN_INT_SCL  GPIO_NUM_0
#define LCD_HOST     SPI3_HOST
#define LCD_SIZE     128
#define LCD_LINES    32           // alto del búfer de dibujo
#define LP5562_ADDR  0x30
#define BRIGHTNESS   80           // %

static const gc9a01_lcd_init_cmd_t gc9107_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb2, (uint8_t[]){0x2f}, 1, 0},
    {0xb3, (uint8_t[]){0x03}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x01}, 1, 0},
    {0xac, (uint8_t[]){0xcb}, 1, 0},
    {0xab, (uint8_t[]){0x0e}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x19}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xe8, (uint8_t[]){0x24}, 1, 0},
    {0xe9, (uint8_t[]){0x48}, 1, 0},
    {0xea, (uint8_t[]){0x22}, 1, 0},
    {0xc6, (uint8_t[]){0x30}, 1, 0},
    {0xc7, (uint8_t[]){0x18}, 1, 0},
    {0xf0, (uint8_t[]){0x1f, 0x28, 0x04, 0x3e, 0x2a, 0x2e, 0x20, 0x00, 0x0c, 0x06, 0x00, 0x1c, 0x1f, 0x0f}, 14, 0},
    {0xf1, (uint8_t[]){0x00, 0x2d, 0x2f, 0x3c, 0x6f, 0x1c, 0x0b, 0x00, 0x00, 0x00, 0x07, 0x0d, 0x11, 0x0f}, 14, 0},
};

#define COLOR_BG     0x000000
#define COLOR_TEXT   0xE0E0E0
#define COLOR_DIM    0x808080
#define COLOR_ACCENT 0xE53935  // rojo del logo
#define COLOR_OK     0x3DDC84
#define COLOR_WARN   0xFFB300
#define COLOR_BAD    0xF44336
#define COLOR_LISTEN 0x29B6F6
#define COLOR_SPEAK  0xFF7043
#define COLOR_AWAY   0xB388FF

static bool s_ready;
static i2c_master_dev_handle_t s_lp5562;
static lv_obj_t *s_main, *s_wifi_icon, *s_wifi_label, *s_bridge_label, *s_ring, *s_state_label, *s_line1, *s_line2;

typedef enum { ANIM_NONE, ANIM_BREATHE, ANIM_PULSE } anim_mode_t;
static anim_mode_t s_anim = ANIM_NONE;

// ---------------------------------------------------------------- hardware

static void backlight_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_INT_SDA,
        .scl_io_num = PIN_INT_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LP5562_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK || i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "LP5562 no disponible: pantalla sin retroiluminación");
        return;
    }
    const uint8_t enable[] = {0x00, 0x40};  // ENABLE: chip_en
    i2c_master_transmit(dev, enable, 2, 100);
    vTaskDelay(pdMS_TO_TICKS(1));
    const uint8_t regs[][2] = {
        {0x08, 0x41},                        // CONFIG: reloj interno, PWM a 558 Hz
        {0x70, 0x00},                        // LED_MAP: todas las salidas por registro I2C
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        i2c_master_transmit(dev, regs[i], 2, 100);
    }
    s_lp5562 = dev;
    display_set_brightness(BRIGHTNESS);
}

void display_set_brightness(int percent)
{
    if (s_lp5562) {
        percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
        const uint8_t reg[] = {0x0E, (uint8_t)(percent * 255 / 100)};  // W_PWM: canal de la retroiluminación
        i2c_master_transmit(s_lp5562, reg, 2, 100);
    }
}

// Los AtomS3R traen GC9107 o ST7735 según el lote: se distingue por RDDID (0x04).
static uint32_t read_panel_id(void)
{
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_LCD_RST) | BIT64(PIN_LCD_CS) | BIT64(PIN_LCD_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_DC, 0);  // fase de comando
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 8,
        .dummy_bits = 1,
        .mode = 0,
        .clock_speed_hz = 16 * 1000 * 1000,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
        .queue_size = 1,
    };
    spi_device_handle_t spi;
    if (spi_bus_add_device(LCD_HOST, &dev_cfg, &spi) != ESP_OK) {
        return 0;
    }
    spi_transaction_t wake = {.length = 8, .flags = SPI_TRANS_USE_TXDATA};
    esp_err_t err = spi_device_polling_transmit(spi, &wake);
    spi_transaction_t rd = {.cmd = 0x04, .rxlength = 32, .flags = SPI_TRANS_USE_RXDATA};
    gpio_set_level(PIN_LCD_CS, 0);
    if (err == ESP_OK) {
        err = spi_device_polling_transmit(spi, &rd);
    }
    gpio_set_level(PIN_LCD_CS, 1);
    spi_bus_remove_device(spi);
    if (err != ESP_OK) {
        return 0;
    }
    return rd.rx_data[0] | rd.rx_data[1] << 8 | rd.rx_data[2] << 16 | (uint32_t)rd.rx_data[3] << 24;
}

static lv_display_t *panel_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_SIZE * LCD_LINES * sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    uint32_t id = read_panel_id();
    bool st7735 = (id & 0xFFFF) == 0x7683 || (id & 0xFFFF) == 0x897C;
    ESP_LOGI(TAG, "panel 0x%08lx: %s", (unsigned long)id, st7735 ? "ST7735" : "GC9107");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    gc9a01_vendor_config_t vendor = {
        .init_cmds = gc9107_init_cmds,
        .init_cmds_size = sizeof(gc9107_init_cmds) / sizeof(gc9107_init_cmds[0]),
    };
    esp_err_t err;
    if (st7735) {
        err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    } else {
        panel_cfg.vendor_config = &vendor;
        err = esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel);
    }
    if (err != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, st7735);
    esp_lcd_panel_mirror(panel, st7735, st7735);
    esp_lcd_panel_set_gap(panel, st7735 ? 2 : 0, st7735 ? 3 : 32);
    esp_lcd_panel_disp_on_off(panel, true);

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;  // por debajo del audio y la red
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        return NULL;
    }
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_SIZE * LCD_LINES,
        .double_buffer = true,
        .hres = LCD_SIZE,
        .vres = LCD_SIZE,
        .rotation = {.mirror_x = st7735, .mirror_y = st7735},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = 1, .swap_bytes = 1},
    };
    return lvgl_port_add_disp(&disp_cfg);
}

// ---------------------------------------------------------------- pantallas

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_BG), 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

// Arco de onda sonora para el logo (0° = derecha, sentido horario).
static void wave(lv_obj_t *parent, int size, int start, int end, lv_opa_t opa)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_align(a, LV_ALIGN_CENTER, 0, -16);
    lv_arc_set_bg_angles(a, start, end);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
}

// Logo propio: un punto con dos ondas a cada lado (una voz que sale) y el nombre debajo.
static lv_obj_t *splash_create(const char *name, const char *version)
{
    lv_obj_t *s = screen();
    lv_obj_t *dot = lv_obj_create(s);
    lv_obj_set_size(dot, 18, 18);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -16);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    wave(s, 44, 320, 40, LV_OPA_COVER);
    wave(s, 44, 140, 220, LV_OPA_COVER);
    wave(s, 72, 325, 35, LV_OPA_60);
    wave(s, 72, 145, 215, LV_OPA_60);

    lv_obj_t *n = label(s, &lv_font_montserrat_14, COLOR_TEXT);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(n, LCD_SIZE - 8);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t *v = label(s, &lv_font_montserrat_10, COLOR_DIM);
    lv_label_set_text_fmt(v, "v%s", version);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -4);
    return s;
}

static void main_create(void)
{
    s_main = screen();
    s_wifi_icon = label(s_main, &lv_font_montserrat_14, COLOR_DIM);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_LEFT, 4, 3);
    s_wifi_label = label(s_main, &lv_font_montserrat_10, COLOR_DIM);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_LEFT, 24, 6);
    s_bridge_label = label(s_main, &lv_font_montserrat_10, COLOR_DIM);
    lv_obj_align(s_bridge_label, LV_ALIGN_TOP_RIGHT, -4, 6);

    s_ring = lv_obj_create(s_main);
    lv_obj_set_size(s_ring, 52, 52);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 5, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);

    s_state_label = label(s_main, &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 84);
    s_line1 = label(s_main, &lv_font_montserrat_10, COLOR_DIM);
    lv_obj_align(s_line1, LV_ALIGN_BOTTOM_MID, 0, -14);
    s_line2 = label(s_main, &lv_font_montserrat_10, COLOR_DIM);
    lv_obj_align(s_line2, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void anim_size(void *obj, int32_t v)
{
    lv_obj_set_size(obj, v, v);
}

static void anim_border_opa(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa(obj, (lv_opa_t)v, 0);
}

static void ring_anim(anim_mode_t mode)
{
    if (mode == s_anim) {
        return;
    }
    s_anim = mode;
    lv_anim_delete(s_ring, NULL);
    lv_obj_set_size(s_ring, 52, 52);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    if (mode == ANIM_NONE) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    if (mode == ANIM_PULSE) {  // Danny habla
        lv_anim_set_exec_cb(&a, anim_size);
        lv_anim_set_values(&a, 44, 60);
        lv_anim_set_duration(&a, 320);
        lv_anim_set_reverse_duration(&a, 320);
    } else {  // escucha
        lv_anim_set_exec_cb(&a, anim_border_opa);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
        lv_anim_set_duration(&a, 900);
        lv_anim_set_reverse_duration(&a, 900);
    }
    lv_anim_start(&a);
}

bool display_init(const char *name, const char *version)
{
    backlight_init();
    if (!panel_init()) {
        ESP_LOGW(TAG, "pantalla no disponible");
        return false;
    }
    if (!lvgl_port_lock(1000)) {
        return false;
    }
    lv_screen_load(splash_create(name, version));
    main_create();
    // El logo queda 2,5 s y luego aparece el estado (la pantalla del logo se libera sola).
    lv_screen_load_anim(s_main, LV_SCREEN_LOAD_ANIM_FADE_IN, 400, 2500, true);
    lvgl_port_unlock();
    s_ready = true;
    return true;
}

// ---------------------------------------------------------------- estado

typedef struct {
    const char *bridge;  // estado del puente
    const char *text;
    uint32_t color;
    anim_mode_t anim;
} state_view_t;

static const state_view_t STATES[] = {
    {"idle", "Listo", COLOR_OK, ANIM_NONE},
    {"connecting", "Conectando", COLOR_WARN, ANIM_BREATHE},
    {"listening", "Escuchando", COLOR_LISTEN, ANIM_BREATHE},
    {"speaking", "Hablando", COLOR_SPEAK, ANIM_PULSE},
    {"farewell", "Despedida", COLOR_AWAY, ANIM_PULSE},
    {"cooldown", "Descanso", COLOR_DIM, ANIM_NONE},
    {"disabled", "Apagado", COLOR_DIM, ANIM_NONE},
    {"error", "Error", COLOR_BAD, ANIM_NONE},
};

static uint32_t rssi_color(int rssi)
{
    return rssi >= -60 ? COLOR_OK : rssi >= -70 ? 0xC0CA33 : rssi >= -78 ? COLOR_WARN : COLOR_BAD;
}

void display_show(const display_info_t *in)
{
    if (!s_ready || !lvgl_port_lock(100)) {
        return;
    }
    state_view_t v = {NULL, "Listo", COLOR_OK, ANIM_NONE};
    switch (in->link) {
    case DISPLAY_NO_CONFIG:
        v = (state_view_t){NULL, "Configurar", COLOR_DIM, ANIM_NONE};
        break;
    case DISPLAY_NO_WIFI:
        v = (state_view_t){NULL, "Sin WiFi", COLOR_WARN, ANIM_BREATHE};
        break;
    case DISPLAY_NO_BRIDGE:
        v = (state_view_t){NULL, "Sin puente", COLOR_WARN, ANIM_BREATHE};
        break;
    case DISPLAY_ONLINE:
        for (size_t i = 0; i < sizeof(STATES) / sizeof(STATES[0]); i++) {
            if (strcmp(in->state, STATES[i].bridge) == 0) {
                v = STATES[i];
                break;
            }
        }
        if (in->playing && v.anim != ANIM_PULSE) {  // p. ej. el pitido de conexión
            v.anim = ANIM_PULSE;
        }
        break;
    }

    bool assoc = in->link >= DISPLAY_NO_BRIDGE && in->rssi != 0;
    uint32_t wifi_color = assoc ? rssi_color(in->rssi) : COLOR_DIM;
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(wifi_color), 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(wifi_color), 0);
    if (assoc) {
        lv_label_set_text_fmt(s_wifi_label, "%d dBm  c%d", in->rssi, in->channel);
    } else {
        lv_label_set_text(s_wifi_label, "--");
    }
    bool online = in->link == DISPLAY_ONLINE;
    lv_label_set_text(s_bridge_label, online ? "HA " LV_SYMBOL_OK : "HA " LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_bridge_label, lv_color_hex(online ? COLOR_OK : COLOR_BAD), 0);

    lv_obj_set_style_border_color(s_ring, lv_color_hex(v.color), 0);
    lv_label_set_text(s_state_label, v.text);
    ring_anim(v.anim);

    if (online) {
        int64_t s = in->online_s;
        if (s < 3600) {
            lv_label_set_text_fmt(s_line1, "conectado %02d:%02d", (int)(s / 60), (int)(s % 60));
        } else {
            lv_label_set_text_fmt(s_line1, "conectado %dh%02dm", (int)(s / 3600), (int)(s / 60 % 60));
        }
    } else {
        lv_label_set_text(s_line1, in->link == DISPLAY_NO_CONFIG ? "USB: help" : "reintentando...");
    }
    lv_label_set_text_fmt(s_line2, "cortes wifi %u  HA %u", (unsigned)in->wifi_drops, (unsigned)in->bridge_drops);
    uint32_t drops = in->wifi_drops + in->bridge_drops;
    lv_obj_set_style_text_color(s_line2, lv_color_hex(drops == 0 ? COLOR_DIM : drops < 3 ? COLOR_WARN : COLOR_BAD), 0);
    lvgl_port_unlock();
}
