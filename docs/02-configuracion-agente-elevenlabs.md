# Configuración del agente Danny en ElevenLabs

## Estado real (2026-10-11, PUBLICADO como "Danny v1.7", TTS V4 Turbo)

Agente: *Horror Hospitality Concierge* · `agent_9401m4hv6mgaecp9jbtbcwwan1qw` · rama `Main`.

| Ajuste | Antes | Ahora (borrador) |
|---|---|---|
| Prompt | Bilingüe ES/EN, con datos del evento | `personas/danny/prompt.md` v1.1: sólo comportamiento (español, brevedad estricta, emergencias, `end_call`) |
| Base de conocimiento | Vacía | 4 docs de `personas/danny/kb/` (evento, Room 237/Sendero, lore ficticio, datos reales de *El resplandor*), todos con *Incluir siempre en el aviso* (sin RAG: sin latencia de recuperación; ~9 kB) |
| Límite de tokens del LLM | −1 | 250 (tope de seguridad; la brevedad la controla el prompt) |
| Primer mensaje | Saludo fijo | `{{saludo}}` (valor de prueba definido en Variables) |
| Idiomas | Español + Inglés | Sólo Español |
| Herramienta Detectar idioma | ON | OFF |
| Herramienta Terminar conversación | OFF | ON |
| Duración máxima | 600 s | 300 s |
| Terminar tras silencio | −1 (nunca) | 60 s (red de seguridad; el puente cortará antes) |
| Mensaje de duración máxima | Genérico | Despedida de Danny |
| Autenticación | OFF (agente público) | ON (signed URL) |
| Overrides | Idioma + Solo texto | Ninguno |
| Ráfagas / cola de llamadas | ON / ON | OFF / OFF |
| Límite diario / concurrente | 100000 / suscripción | 300 / 2 |

Sin cambios (ya correctos o a decidir con mediciones): formato entrada/salida `PCM 16000 Hz`, voz *Tom - Concierge de Terror* con TTS **V4 Turbo** (~83 ms, el más rápido de la lista), LLM *DeepSeek Flash 4.1*, ASR *Scribe v2 Realtime*, turno V2 con *turn timeout* 7 s, turno especulativo ON, filtro de habla de fondo OFF (probar en H6 con ruido real), *Turn V3* sin activar (comparar con `el_spike`).

Verificado tras publicar (llamada de prueba de 15 s en la vista previa): responde en español aunque se le hable en inglés, 2–3 oraciones, usa frases de la KB sin revelar Room 237. Sin `saludo` la sesión falla con `Missing required dynamic variables in first message: {'saludo'}` → el puente y `el_spike` siempre lo envían.

**Voz (resuelto en v1.2):** en v1.1 el saludo lo dijo *Tom - Concierge de Terror* y las respuestas la voz adicional *ManuelSpain*. Se quitó ManuelSpain del agente (sigue en la biblioteca de voces); única voz: Tom.

Pendientes detectados en el panel: dos herramientas "Herramienta desconocida / no encontrada" referenciadas por el agente (eliminar si no se usan); ~~voz adicional ManuelSpain~~ (quitada en v1.2).

Lista de verificación para dejar el agente listo para `tools/el_spike` y, después, para el puente.
Los nombres de las pestañas del panel cambian con frecuencia: si alguno no coincide, buscar el ajuste por su nombre técnico (entre paréntesis, ruta en la API/CLI cuando está documentada).

## 1. Identidad y voz

- [ ] Voz: la voz de Voice Design de Danny.
- [ ] Idioma por defecto: **español**. No añadir idiomas adicionales; desactivar detección/cambio automático de idioma.
- [ ] Modelo de TTS: probar primero el de **menor latencia** que soporte español (familia *Flash*) y comparar con uno de mayor calidad. Elegir con `el_spike` midiendo `PRIMER AUDIO` y escuchando `danny.wav`. Documentar aquí el elegido: `________`.
- [ ] LLM: uno rápido (la latencia del primer token domina). Documentar: `________`.
- [ ] System prompt: `personas/danny/prompt.md` (completar los `[[ ]]`).

## 2. Primer mensaje con saludos variados

- [ ] **First message**: `{{saludo}}`
- [ ] Declarar la variable dinámica `saludo` con un valor de prueba (para el probador del panel), p. ej. «Bienvenidos al Hotel Overlook. ¿Es su primera visita?».
- El puente/`el_spike` envía el valor real en `conversation_initiation_client_data.dynamic_variables` (no requiere habilitar *overrides*).

## 3. Audio

- [ ] Formato de entrada del usuario: **PCM 16000 Hz** (`pcm_16000`).
- [ ] Formato de salida del agente: **PCM 16000 Hz** (`pcm_16000`).
- `el_spike` verifica ambos en `conversation_initiation_metadata` y aborta si no coinciden.

## 4. Turnos y duración

- [ ] *Turn timeout* (`conversation_config.turn.turn_timeout`, 1–30 s): **7 s** inicial.
- [ ] *Max conversation duration* (`conversation_config.conversation.max_duration_seconds`, 60–7200 s, defecto 600): **300 s** como red de seguridad (el puente cortará antes).
- [ ] Herramienta de sistema **End call** habilitada.
- [ ] Interrupciones: dejar el valor por defecto. En la fase E0 el terminal envía silencio mientras Danny habla, así que no habrá auto-interrupciones.

## 5. Seguridad y costos

- [ ] **Autenticación activada** (agente privado, *signed URL*). Con esto un `agent_id` filtrado no sirve para abrir sesiones.
- [ ] **No** configurar allowlist de dominios al mismo tiempo (la doc lo desaconseja con signed URL).
- [ ] *Overrides*: todos desactivados.
- [ ] Límite de llamadas concurrentes del agente: **1–2** (sólo hay un terminal). Desactivar *burst* si el panel lo permite.
- [ ] API key dedicada al proyecto, con permisos mínimos y límite de crédito si el plan lo permite. Guardarla sólo en `tools/el_spike/.env` (local) y luego en el puente.

## 6. Base de conocimiento (opcional, Hito 3)

- [ ] Documento corto con datos operativos REALES del evento (horarios, votación, servicios). Nada de secretos de Room 237 ni del Sendero.

## 7. Verificación con `el_spike`

```bash
cd tools/el_spike && ./make_test_wavs.sh
```

```bash
cd tools/el_spike && uv run el_spike.py --saludos ../../personas/danny/saludos.txt --wav wavs/01-primera-visita.wav --wav wavs/02-room-237.wav --wav wavs/05-operativa.wav
```

Criterios de aceptación:

| Comprobación | Esperado |
|---|---|
| `formatos` | `entrada=pcm_16000 salida=pcm_16000` |
| Saludo | Uno de `saludos.txt`, con la voz de Danny |
| 02 Room 237 | Responde con misterio, sin revelar contenido |
| 05 operativa | No inventa horario si no está en el prompt |
| 06 miedo | Suaviza el tono |
| 07 inglés | Responde en español |
| 08 despedida | Se despide y la sesión termina (`cierre.by_server = true` si usó end_call) |
| `PRIMER AUDIO` | Anotar mediana de varios turnos; objetivo < 1500 ms |
| Costo | La estimación es coherente con la duración |

## API key del proyecto (creada por el usuario)

Nombre sugerido: `Horror Concierge`. Mínimo privilegio:

| Permiso | Valor | Motivo |
|---|---|---|
| ElevenAgents | **Escribir** | Pedir signed URL (`GET /v1/convai/conversation/get-signed-url`). Verificado 2026-10-11: con *Leído* responde 401 `missing the permission convai_write`. |
| Todo lo demás | Sin acceso | No se usa TTS/STT directo; la conversación la factura el agente. |
| Límite de créditos | p. ej. 20 000 en pruebas | Red de seguridad; puede no contar los créditos de la conversación (se facturan al agente). El control real son los límites del agente. |
| Caducidad | Después del evento (o 90 días) | Rotar para reutilizar el proyecto. |
| Restringir por IP | Sólo si el puente sale con IP pública fija | |
| Desactivación automática si se filtra | ON | |

## Resultados de `el_spike` (2026-10-11, agente v1.2, sin hardware)

Sesión `conv_9001m4q4164ce5x94acn1avwgmfz`, 6 preguntas con voz sintética de macOS, región por defecto.

| Métrica | Valor |
|---|---|
| Signed URL / WebSocket abierto / primer audio del saludo | 0,36 s / 0,95 s / 1,18 s desde el arranque |
| Ping (RTT servidor) | 126–128 ms |
| Fin de voz → primer audio de Danny | 0,92 · 1,10 · 1,19 · 1,88 · 2,17 · 2,41 s (mediana ≈ 1,5 s) |
| Transcripción vs. primer audio | Llegan casi juntos → domina la detección de fin de turno, no el LLM ni el TTS |
| Duración de respuestas (audio) | 8–18 s → **demasiado largas** → prompt v1.3 (máx. 25 palabras) |
| Idioma | Español en todos los turnos, incluida la pregunta en inglés ✔ |
| Datos del evento | No inventó horario ✔ · usó lore y frases de la KB ✔ · no reveló Room 237 ✔ |
| Miedo | Suavizó el tono ✔ |
| Despedida | Correcta; la sesión la cortó el servidor por **cuota agotada** (`1002 This request exceeds your quota limit`), no por `end_call` → repetir |

Observaciones:
- El texto trae etiquetas de audio (`[charming]`, `[mysterious]`…) del *Modo expresivo* de la voz. Escuchar `danny.wav` para confirmar que el TTS las interpreta y no las pronuncia.
- Preguntaba el nombre en casi todos los turnos → corregido en v1.3.
- Bug de la prueba corregido: sin emular la reproducción en tiempo real, el script hablaba encima de Danny y provocaba `interruption`.

Pendiente: repetir con v1.3 tras tener créditos; probar `Turn V3` y disposición de turno "ansiosa" para bajar la latencia de fin de turno.

## Cuenta

Plan **Free**: 10 000 créditos/mes (≈15 min de llamadas), **agotados** el 2026-10-11 por las pruebas. Además, el plan Free no incluye licencia comercial. Para el evento (≈120 min estimados + pruebas) el plan razonable es **Creator** (275 min/mes); Starter (75 min) se queda corto.

## Resultados con créditos (2026-10-11)

| Versión | Sesión | Fin de voz → primer audio | Audio por respuesta | Preguntas al final | Cierre |
|---|---|---|---|---|---|
| v1.3 | `conv_4701m4q4eh9fesqa82p770d58rcx` (7 turnos) | 0,92–2,47 s, mediana 1,32 s | 6–17 s | 6 de 7 | `end_call` ✔ (1000, servidor) |
| v1.4 | `conv_8601m4q4qez7ffrrdpq628csh6hv` (5 turnos) | 0,90–3,74 s, mediana 1,09 s | 5–11 s | 0 de 5 | `end_call` ✔ (1000, servidor) |

v1.4 = menos preguntas, trato neutro en género ("estimados huéspedes" en lugar de "caballero"), 8 palabras clave ASR (Overlook, Room 237, Sendero del Terror, DJ Acetato, Otocuma, disfraces, El resplandor, Danny; máx. 20 caracteres c/u).

- En v1.3 el ASR transcribió "¿A qué hora es el concurso…?" como "Aquí ahora es el concurso…" y Danny respondió otra cosa; en v1.4 (con palabras clave) se transcribió bien. Con voz real y ruido habrá más errores: probar en H6.
- Danny habla lento (~2,7 palabras/s por las pausas expresivas): 25 palabras ≈ 9–10 s. Si se quiere más ágil: subir la velocidad de la voz (p. ej. 1,1) o bajar el límite a ~18 palabras.
- La despedida tardó 3,7 s (decide además llamar a `end_call`).
- Las etiquetas de audio ahora salen en español (`[amable]`, `[con calma]`): confirmar escuchando que no se pronuncian.

## Voz "Demonio Antiguo" y tono siniestro (v1.5, 2026-10-11)

Problema: la voz sonaba más aterradora en Voice Design que en el agente y "muy amable".

Causas encontradas:
- Las **etiquetas de audio sugeridas** venían de una plantilla de soporte (*Patient* = "when a customer is frustrated", *Excited* = "when the customer provides positive feedback") y el LLM añadía `[happy]`, `[amable]`, `[warm]`: el TTS obedecía y sonaba amable.
- El prompt describía a Danny como *charming/warm* sin indicar entrega vocal.
- Modelo **V4 Turbo** (el más rápido) en lugar del modelo expresivo de la familia v3.
- Estabilidad 50%.

Cambios:
- Etiquetas: eliminadas *Patient* y *Excited*; quedan *Suspira* + nuevas `sinister laugh` (temas de terror: risa grave, lenta, aterradora, máx. 1 por respuesta), `whispers` (secretos/frase más inquietante), `menacing` (insinuaciones amenazantes).
- Prompt v1.5: sección *Delivery and audio tags* (voz grave, lenta, amenaza educada, sin palabras alegres ni exclamaciones, sólo esas 4 etiquetas, risa en temas de terror, se suspende ante miedo real o emergencia).
- Estabilidad 34% (sólo aplica a V4/V4 Turbo; con V3 los ajustes de voz no son editables).
- A/B de modelo con las mismas 3 preguntas: `out/A-v4-turbo-demonio.wav` vs `out/B-v3-conversacional-demonio.wav`. Elección del usuario: **V4 Turbo** ("más natural"); publicado de nuevo con estabilidad 34%.

Medición B (V3): primer audio 0,95 / 2,30 / 1,25 s; saludo 1,87 s tras conectar (V4 Turbo: 1,15 s). Respuestas 8–13 s.

Otras palancas si aún no suena suficientemente aterrador:
- Volver a Voice Design y generar la voz pensando en v3 (descripción con "susurro grave, respiración áspera, risa lenta").
- Formato de salida `pcm_22050`/`pcm_24000`: más agudos y textura de respiración (hoy 16 kHz corta todo sobre 8 kHz). Requiere I2S del Atom a esa frecuencia o remuestreo en el puente.
- Postproceso en el puente (bajar tono 1–2 semitonos, reverb corta, leve distorsión) — [EXPERIMENTAL], añade unos ms de latencia, determinista y sin costo.
- Parlante con buena respuesta en graves dentro de la figura.

## Base de conocimiento refinada (v1.6, 2026-10-11)

Datos del usuario y del sitio oficial room237.detrasdelpalo.com (páginas públicas /, /evento, /reglas): sede Hotel & Spa Poco a Poco (Monteverde) transformado en el Overlook; Otocuma Cinema Lounge = speakeasy de cine dentro del hotel; Detrás del Palo = "el secreto mejor guardado de Monteverde" (sólo se habla de Otocuma); fecha sábado 31/10/2026, 8 p. m.–1 a. m.; quien habla con Danny ya pagó; inscripción/votación/premios/criterios/reglas del concurso; WhatsApp +506 8484 5208; QR en el pecho de Danny; @detrasdelpalodp; advertencias de las atracciones; recomendación del botón en la boca del león; sin menú aún.

Los documentos de la KB se editan en el panel (Editar → Guardar) y aplican de inmediato (son del workspace); el prompt sí requiere Publicar.

Prueba `conv_3801m4q7hkm1f328xte07fzjy2xj` (6 preguntas nuevas): todas correctas (lugar, speakeasy, pasos de inscripción, premios, votación, botón del león). Primer audio 0,98–1,49 s. Respuestas de 13–18 s (preguntas informativas). Al listar premios omitió el Favorito del público.

## v1.7 (2026-10-12)

DJ = DJ Ritmos Mulatos (mezcla con discos de acetato); show de Halloween "Thriller", tributo al Rey del Pop; premios siempre completos (los cuatro, con monto, hotel y lugar; excepción de brevedad ~70 palabras). Palabras clave ASR: −"DJ Acetato", +"Ritmos Mulatos", +"Thriller" (9/50).

Prueba `conv_8501m4q4qd6w6kez29kxzee5p3wsqx`: premios completos con los 4 hoteles ✔ (30,5 s de audio), DJ ✔, show ✔. Primer audio 1,13–1,48 s.

## Hallazgos del puente con el agente real (2026-10-12)

- **Despedida:** enviar una "nota del sistema" como `user_message` activó la protección *Prompt Injection* del agente y ElevenLabs cortó la llamada (`1008 Conversation was stopped because the 'Prompt Injection' guardrail was triggered`). El puente ahora envía una frase natural de visitante ("Bueno, muchas gracias, ya nos tenemos que ir."): Danny se despide y llama a `end_call` (cierre 1000 del servidor). Verificado en `conv_0101m4qef225eg2r3jg0ec4c10w4`.
- **"Tomar turno tras el silencio" = 7 s:** cuando los visitantes callan, Danny pregunta "¿siguen ahí?" con una transcripción vacía ("...") del lado del usuario. El puente ignora esas transcripciones y no reinicia la inactividad con esos recordatorios.
- **"Terminar conversación tras el silencio" = 60 s:** red de seguridad del agente; el puente se despide antes (20 s de inactividad por defecto).

## v1.8 (2026-10-29): Hotel Poco a Poco, invitados externos

Aplicado por API (`PATCH /v1/convai/agents/{id}`; versión `agtvrsn_6201m64kd18efqzrry4fw3ggqtwq`), sin tocar LLM, voz, turnos ni herramientas:

- Prompt v1.8: el hotel es el Hotel & Spa Poco a Poco; el Overlook sólo como el hotel de *El resplandor* cuando pregunten por la película. Se nombran Detrás del Palo, Otocuma Cinema Lounge y la Room 237 Halloween Party. Los visitantes son **invitados** externos del evento, no huéspedes: nada de hospedaje, habitaciones, check-in ni reservas (si preguntan, el personal del hotel).
- KB: documentos nuevos (v1.8) para evento, Room 237/Sendero y lore (ahora "Lore ficticio del Hotel Poco a Poco"); "El resplandor — datos reales" sin cambios. Los documentos v1.7 quedan en el workspace sin usar (`NYvaz0f96LVM72DKR78e`, `RdFBc90a6DFVhW53l8OZ`, `IMJIRlrsCAtIet1ty0ID`); se pueden borrar desde el panel.
- Saludo de prueba del panel y palabras clave ASR: −"Overlook", +"Poco a Poco", +"Detrás del Palo" (10/50).
- Saludos y despedidas sin Overlook ni "huéspedes"; el puente los incluye en la imagen (app 0.4.1).

Prueba `conv_3801m64ke6djf2facj35rqahs689`: "¿Este es el hotel Overlook de la película?" → "No, estimados invitados; este es el Hotel Poco a Poco… El Overlook es el hotel de la película El resplandor" ✔. "¿Nos podemos quedar a dormir?" → son invitados del evento, el personal del hotel los atiende ✔. "¿Qué es Detrás del Palo?" ✔.

## v1.9 (2026-10-30): cócteles de autor, jurado invitado, concurso nuevo

Aplicado desde el panel (Chrome), sin tocar LLM, voz ni turnos:

- Prompt v1.9: Danny puede recomendar de vez en cuando un cóctel de autor de Otocuma (sin presionar a beber); al pedir el menú nombra los cuatro y describe uno por respuesta. Concurso con dos caminos: premios del jurado por inscripción en el stand de los jueces (preselección y tres ganadores), Favorito del público por QR o búsqueda por nombre en la plataforma. Jueces = personas reales: respeto, sin lore ni bromas.
- KB: documento nuevo "Cócteles de autor de Otocuma [v1.9]" (*Incluir siempre en el aviso*), desde `kb/05-cocteles-de-autor.md` (fichas técnicas; "Sangre de padre" sin perfil sensorial y "Hechizo del campo" sin garnish: Danny no los inventa). "Evento Room 237" editado con el concurso nuevo y el jurado (Joss Araya, Andy Valverde, Steph Molina). "Room 237 y Sendero": cóctel como alternativa si alguien tiene miedo.
- Términos clave ASR (17/50): +"Joss Araya", +"Andy Valverde", +"Steph Molina", +"Bruma de Monte Verde", +"Sangre de padre", +"Hechizo del campo", +"Fantasma de cosecha" (máx. 20 caracteres).
- El sitio (/reglas) aún dice que para la preselección del jurado "no hace falta hacer nada"; la KB sigue la instrucción del organizador (inscripción en el stand).

Prueba en chat de vista previa: primer lugar → stand de jueces, preselección, tres ganadores ✔; Favorito del público → Mi expediente, QR o nombre ✔; jurado → presenta a Joss Araya ✔; cócteles → nombra los cuatro y recomienda El fantasma de la cosecha con su leyenda ✔. Pendiente: sigue usando "estimado invitado" en singular masculino a veces.

## v1.10 (2026-10-30): trato individual tétrico y sin género

En v1.9 seguía diciendo "estimado invitado" a una sola persona. Ahora, en singular, usa sustantivos de género gramatical fijo y tono tétrico ("estimada víctima", "alma valiente", "pobre alma", "criatura de la noche"), el pronombre "le" en vez de "lo"/"la", y nunca formas de lenguaje inclusivo ("elle", "todes", "invitade", "x", "@"). En grupo sigue "estimados invitados".

Prueba en chat de vista previa: "criatura de la noche", "alma valiente", "el personal le acompaña", "le esperan", "un ojo de lichi que le observará" ✔.
