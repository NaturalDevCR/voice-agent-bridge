# Firmware del terminal

ESP-IDF 6.1 para AtomS3R + Atomic Audio-3.5 Base. Conecta por Wi-Fi al puente
(**Voice Agent Bridge**) con el protocolo v0 y sólo mueve audio y el botón: el puente decide
cuándo hay conversación, habla con ElevenLabs y con Home Assistant.

## Compilar y flashear

```bash
. ~/esp/esp-idf/export.sh
cd firmware/terminal
idf.py set-target esp32s3   # sólo la primera vez
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Salir del monitor: `Ctrl+]`.

## Configurar (una vez, se guarda en el Atom)

En el monitor, con el prompt `terminal>`:

```
wifi "Nombre de la red" "clave"
url ws://<IP de Home Assistant>:8765/v0/terminal
token <token del terminal>
reboot
```

- El token está en Home Assistant → Ajustes → Aplicaciones → Voice Agent Bridge →
  Configuración → *Token del terminal* (o en el Registro si se generó solo).
- Sin `?id=` en la URL el terminal se presenta como `atom-<últimos 3 bytes de la MAC>`.
- `status` muestra Wi-Fi, RSSI, conexión con el puente, estado de la sesión y niveles de audio.
- `vol` y `gain` se guardan. Por defecto 100 y 30 dB (medidos en el Hito 1, docs/03).
- Para el evento: cambiar sólo `wifi` a la red del lugar y la IP en `url`.
- Ojo: `wifi off` no apaga la radio (eso es `radio off`); el firmware lo rechaza para no
  guardar una red llamada "off".

Otros comandos (`help` los lista todos):

| Comando | Qué hace | Se guarda |
|---|---|---|
| `scan [all]` | APs de la red con canal y señal (`→` = el actual); `all` = todas las redes | — |
| `name "<nombre>"` | Nombre bajo el logo (sin tildes: la fuente es ASCII) | sí |
| `eq <graves> <agudos>` | Ecualizador de salida en dB (estantes 250 Hz / 3 kHz) | sí |
| `txpower [dBm]` | Potencia máxima de Wi-Fi, 2–20; por defecto 20 (máximo alcance) | sí |
| `mute auto\|on\|off` | Silencio de DAC y amplificador; `auto` = cuando no suena nada | no |
| `beep [Hz] [ms]` | Tono generado en el Atom (no usa la red) | — |
| `playurl <url.wav>` | Reproduce un WAV 16 kHz por HTTP (diagnóstico de la salida) | — |
| `radio off\|on`, `backlight <0-100>`, `amp on\|off` | Diagnóstico de ruido; `radio on` reinicia | no |

Credenciales: viven sólo en la NVS del Atom; nunca en el código ni en `sdkconfig`.

## Comportamiento

| Señal | Significado |
|---|---|
| Pitido agudo corto | Conectado al puente |
| Dos pitidos graves | Botón presionado sin conexión al puente |
| Botón (pantalla) | Inicia la conversación; si hay una en curso, el puente la termina |

- Micrófono: sólo cuando el puente lo pide (`{"t":"mic","on":true}`), en paquetes de 100 ms
  (5 tramas de 20 ms; el puente acepta cualquier tamaño).
- Half-duplex E0: mientras suena el agente y 250 ms después, envía silencio en lugar del
  micrófono (el puente aplica la misma regla por su lado).
- Reproducción: búfer de 30 s en PSRAM, arranca con 100 ms acumulados; `flush` lo vacía.
- `stats` cada 5 s al puente: RSSI, memoria, vaciados/desbordes del búfer, tramas perdidas, nivel del mic.
- Pantalla (LVGL; panel GC9107 o ST7735 detectado por RDDID): logo propio 2,5 s y luego
  señal Wi-Fi (dBm y canal, color por calidad), enlace con HA, estado de la conversación
  (anillo de color, late al escuchar, pulsa al hablar), tiempo de conexión y cortes de
  Wi-Fi/HA desde el arranque.
- Wi-Fi: escaneo de todos los canales y conexión al AP más fuerte (ESP-IDF por defecto toma el
  primero que encuentra), 802.11k/v, y roaming propio cada 30 s si la señal baja de −70 dBm
  y no hay conversación.

## Ruido de la radio en el audio

Cada transmisión Wi-Fi mete un pico de corriente en la alimentación que el amplificador de la
base hace audible (confirmado: con `radio off` desaparece). Mitigaciones, juntas lo dejan
inaudible con parlante:

1. DAC y amplificador (PW_AMP) apagados mientras no suena nada, como hace xiaozhi.
2. Potencia de transmisión: 20 dBm por defecto por alcance. Si con buena señal vuelve el ruido
   mientras habla, bajarla (`txpower 14`).
3. Micrófono en paquetes de 100 ms: 10 ráfagas de radio por segundo en vez de 50.
4. Al perder el Wi-Fi o el puente el audio pendiente se descarta y DAC y amplificador se apagan
   al instante: si no, la radio escaneando para reconectar suena como estática muy fuerte.

## Particiones

`partitions.csv`: NVS + dos slots de app de 3.5 MB (listo para OTA en H7).
