# hw_test — Hito 1

Firmware ESP-IDF 6.1 mínimo para validar el AtomS3R + Atomic Audio-3.5 Base antes del
firmware del terminal. No usa Wi-Fi ni ElevenLabs.

## Compilar y flashear

```bash
. ~/esp/esp-idf/export.sh
cd firmware/hw_test
idf.py set-target esp32s3   # sólo la primera vez
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Salir del monitor: `Ctrl+]`.

## Consola

| Comando | Qué hace | Prueba (docs/01 §6.1) |
|---|---|---|
| `scan` | Escanea bus de audio (38/39) e interno (45/0) | 1.2 |
| `tone [Hz] [dBFS] [s]` | Seno; por defecto 1 kHz, −20 dBFS, 3 s | 1.3 |
| `vol [0-100]` | Volumen del ES8311 (arranca en 40) | 1.3 |
| `amp on\|off` | PW_AMP (P0 del PI4IOE) | 1.4 |
| `rec [s]` / `play` | Graba en PSRAM (máx. 10 s) y reproduce | 1.5 |
| `meter [s]` | Nivel del micrófono cada 200 ms | 1.5 |
| `gain [dB]` | Ganancia del micrófono (arranca en 30) | 1.5 |
| `duplex [s] [dBFS]` | Tono mientras graba; mide acoplamiento de eco | 1.7 |
| `info` | Chip, PSRAM, heap | — |

Botón del Atom: graba 5 s y los reproduce.

**Audífonos:** el nivel de salida del jack no está especificado. Empieza con los audífonos
fuera de los oídos y sube `vol` poco a poco.

## Respaldo de fábrica

Antes del primer flasheo se leyó la flash completa a `backups/` (fuera de git). Restaurar:

```bash
esptool --port /dev/cu.usbmodem2101 write-flash 0 backups/atoms3r-fabrica-7ce8b1e30424.bin
```
