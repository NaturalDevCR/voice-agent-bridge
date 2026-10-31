# Changelog

## 0.5.1

- Si el terminal pierde la conexión en plena conversación, el puente espera hasta 20 s a
  que vuelva antes de cerrarla: un corte breve de Wi-Fi ya no reinicia la conversación
  (el silencio del corte tampoco cuenta como inactividad).

## 0.5.0

- Nuevo botón **Siguiente grupo** (`button.<nombre>_siguiente_grupo`, topic `cmd/restart`):
  corta la conversación en el acto, sin despedida, descarta el audio pendiente y empieza
  otra con un saludo nuevo, sin pausa entre grupos. Sin conversación activa, inicia una.

## 0.4.1

- Saludos de Danny actualizados: el hotel es el Hotel & Spa Poco a Poco (ya no "el Overlook")
  y los visitantes son invitados del evento, no huéspedes.

## 0.4.0

- **Nombre del dispositivo** neutro por defecto ("Asistente de voz" →
  `sensor.asistente_de_voz_estado`), separado del **Personaje** (`persona`, p. ej. `danny`),
  que sólo elige los saludos incluidos. Cambiar de personaje ya no cambia los entity_id.
- Si cambias el nombre del dispositivo, la app borra de Home Assistant las entidades del
  nombre anterior (antes quedaban huérfanas).
- Repositorio renombrado a `NaturalDevCR/voice-agent-bridge` (la URL anterior redirige).

## 0.3.0

- Renombrada a **Voice Agent Bridge**: sirve para cualquier agente, no sólo Danny.
- Nuevo campo **Nombre del agente**: define el dispositivo en Home Assistant, el prefijo
  de entidades (`danny` → `sensor.danny_estado`) y los topics MQTT (`voice_agent/danny/…`).
- Saludos incluidos según el nombre (Danny trae los suyos).

## 0.2.0

- Primera versión como app de Home Assistant (antes "Danny Bridge").
