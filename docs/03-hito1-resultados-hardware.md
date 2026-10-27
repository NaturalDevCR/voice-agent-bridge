# Hito 1 — resultados de hardware

Fecha: 2026-10-27 · Firmware: [`firmware/hw_test`](../firmware/hw_test/README.md) (ESP-IDF 6.1)
· Unidad: AtomS3R MAC 7c:e8:b1:e3:04:24 + Atomic Audio-3.5 Base · Salida: audífonos en el jack.

| # | Prueba | Resultado | Estado |
|---|---|---|---|
| — | Chip | ESP32-S3-PICO-1 rev 0.2, flash 8 MB, PSRAM 8 MB octal; consola USB-Serial/JTAG (`/dev/cu.usbmodem*`) | CONFIRMADO |
| 1.2 | I2C | Bus audio 38/39: 0x18 (ES8311) y 0x43 (PI4IOE, reg 0x01 = 0xA0). Bus interno 45/0: 0x30 (LP5562) y 0x68 (IMU). Lecturas sueltas en 0x22 y 0x0b mientras corre el I2S | CONFIRMADO (0x22/0x0b sin explicar, no afectan) |
| — | ES8311 sin MCLK a 16 kHz | Funciona: I2S estándar 16 bit, BCLK 512 kHz, el códec toma BCLK×8 como MCLK interno | CONFIRMADO |
| 1.3 | Tono por jack | 1 kHz −20 dBFS sin errores I2S. Con audífonos hace falta `vol 100` para nivel cómodo: el jack da poca salida | CONFIRMADO (oído humano, sin medición) |
| 1.4 | PW_AMP (P0) | Con P0 en OFF el jack sigue sonando igual ⇒ el jack no depende de P0 | CONFIRMADO |
| 1.5 | Micrófono | **Selector en ON obligatorio** con audífonos sin micrófono (en OFF graba silencio, −73 dBFS). Ganancia 30 dB: voz a 30–50 cm −20…−27 dBFS, picos −7.6 dBFS; reposo −44 dBFS (cuarto con ventilación/PC). Tasa real 16 kHz, 0 errores | CONFIRMADO |
| 1.6 | Micrófono → PC | Se cubre con el firmware del terminal contra el puente real | PENDIENTE |
| 1.7 | Duplex / eco | Pospuesto a H6: con audífonos no hay acoplamiento que medir | PENDIENTE |
| 1.8 | Estabilidad 60 min | Con el firmware del terminal | PENDIENTE |
| 1.9 | Micrófono externo | Sin micrófono TRRS disponible | PENDIENTE |

## Decisiones para el firmware del terminal

- 16 kHz duplex en un solo puerto I2S, ES8311 vía `esp_codec_dev` con `use_mclk = false`.
- Ganancia de micrófono por defecto 30 dB. No subir: los picos de voz ya llegan a −7.6 dBFS.
- Volumen de salida configurable desde el puente (`{"t":"volume"}`); arranque en 100 mientras la salida sea el jack.
- P0 (PW_AMP) no silencia el jack: sirve para apagar sólo el altavoz interno si se usa parlante externo.
- Pantalla: `hw_test` no la inicializa (queda negra). El botón es la pantalla entera (GPIO41).

## Riesgo anotado

El ruido de reposo (−44 dBFS) queda ~20 dB bajo la voz en un cuarto callado. Con DJ la relación
empeora mucho: en H6 decidir entre hablar cerca del Atom, micrófono externo o *noise gate* en el puente.
