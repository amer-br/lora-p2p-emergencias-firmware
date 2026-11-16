# Firmware del T-Beam — pasarela y módem

Firmware para el **LilyGO T-Beam v1.2** (chip de energía **AXP2101**) de la red LoRa de emergencias. Tiene dos modos, que comparten el radio, la malla y la pantalla, y solo cambian el enlace:

| Modo | Para qué | Enlace | Compilar y grabar |
|------|----------|--------|-------------------|
| **Pasarela** | Nodo fijo del panel de coordinación | USB serie con el puente de radio | `pio run -e pasarela -t upload` |
| **Módem** | Nodo de bolsillo de cada persona | Bluetooth LE con la app del teléfono | `pio run -e modem -t upload` |

```
App ─BLE─► T-Beam módem ))) LoRa 915 MHz ((( T-Beam pasarela ─USB─► puente_radio.py ─WebSocket─► panel
```

## Qué hacen los dos modos

- **Encienden el radio:** el AXP2101 debe activar su salida ALDO2, que alimenta el transceptor; si no, el radio no responde. El GNSS (ALDO3) queda apagado, porque la app usa la ubicación del teléfono.
- **Detectan el transceptor:** el T-Beam v1.2 se vende con **SX1262 o SX1276**. El firmware prueba los dos al arrancar y lo informa por el puerto serie:
  ```
  # Nodo listo (MÓDEM): SX1276, 915.0 MHz, SF9, BW 125 kHz, CR 4/5, 17 dBm
  ```
- **Retransmiten como cualquier nodo** (inundación controlada):
  - descartan duplicados por Msg_ID (recuerdan los últimos 128 durante 10 min);
  - si la trama llega con TTL > 1, la reenvían con el TTL reducido en uno, tras un retardo aleatorio de 100 a 600 ms;
  - antes de transmitir escuchan el canal y, si está ocupado, esperan.
- **No interpretan el contenido** (SRS, supuesto S1). El CRC-16 del protocolo lo validan el panel y la app; el firmware solo usa el CRC propio de LoRa para descartar paquetes dañados.
- **Informan su estado:** cada 30 s imprimen por el puerto serie una línea `# Estado: …` con sus contadores y la batería. Las líneas que empiezan con `#` son informativas y el puente las ignora.

## Modo pasarela: contrato con el puente

Una línea JSON por mensaje, a 115 200 baudios (ver también `PanelCampus/puente_radio/README.md`):

| Sentido | Línea |
|---------|-------|
| T-Beam → puente | `{"hex":"0BADCAFE…","rssi":-102,"snr":-3.0}` por cada trama nueva escuchada |
| Puente → T-Beam | `{"hex":"…"}`: trama por transmitir |
| Puente → T-Beam | `{"ping":1}`: latido mientras el panel está conectado |

## Modo módem: contrato Bluetooth con la app

El módem se anuncia como **`LoRa-XXXX`** (los dos últimos bytes de su MAC; el nombre aparece en su pantalla) con este servicio GATT. La app lo implementa en `Aplicacion/lib/services/gatt_modem.dart`.

| Característica | UUID | Operación | Contenido |
|----------------|------|-----------|-----------|
| Servicio | `8f1d0001-6b3a-4c2e-9e5d-2a7c4b9e1f00` | — | — |
| TX_FRAME | `8f1d0002-…` | Escritura con respuesta | Trama binaria por transmitir (alerta 56 B, acuse 20 B o Safety Check 17 B) |
| RX_FRAME | `8f1d0003-…` | Notificación | `[RSSI int16 BE][SNR int8 en cuartos de dB]` + trama recibida |
| STATUS | `8f1d0004-…` | Lectura y notificación | 9 bytes: versión (1), banderas (bit0 radio lista, bit1 cargando), batería % (0xFF sin batería), Msg_ID de la última trama escrita (BE), resultado (0 encolada, 1 inválida, 2 cola llena), transceptor (1 SX1262, 2 SX1276) |

- **Confirmación de envío:** después de escribir en TX_FRAME, la app espera hasta 2 s a que STATUS notifique el Msg_ID de su trama con resultado 0. Si no llega, la trama queda Pendiente y se reenvía con el mismo Msg_ID; la red descarta el duplicado.
- **MTU:** una trama de 56 B más 3 B de metadatos no cabe en los 20 B por omisión de BLE. La app pide un MTU de 512 al conectarse (Android); iOS negocia uno suficiente por sí solo.
- **Un teléfono a la vez:** al conectarse uno, el módem deja de anunciarse, y vuelve a hacerlo cuando ese teléfono se desconecta.
- **Emparejamiento:** desactivado por ahora (`BLE_REQUIERE_CIFRADO 0` en `include/config.h`) para facilitar las primeras pruebas. La SRS (3.1.4) pide el emparejamiento estándar; hay que activarlo antes de los simulacros.

## Pantalla

La pantalla OLED de 128×64 sigue la estructura visual de la app móvil:

![Pantallas de la pasarela y del módem](docs/pantalla.png)

| Zona | Qué muestra | Equivalente en la app |
|------|-------------|-----------------------|
| Barra superior | «LoRa P2P», antena y el modo; el módem agrega su batería | Barra verde superior |
| Franja de estado | Pasarela: ● Panel conectado / ○ Panel sin conexión. Módem: ● Teléfono conectado / ○ Sin teléfono | Franja «● Módem conectado» |
| Tarjeta | Antes de recibir algo: el radio y, en el módem, el nombre Bluetooth. Después: la última trama escuchada con su categoría, chip de prioridad (ROJA, AMARILLA, VERDE, ACUSE, CENSO), RSSI, SNR y antigüedad. Una alerta de prioridad roja invierte la tarjeta durante 30 s | Tarjeta de incidente con «Prioridad ROJA» |
| Barra inferior | Tramas recibidas (Rx), transmitidas (Tx) y retransmitidas (Ret) | Barra de navegación |

- «Panel conectado» depende del **latido** del puente: si pasan 25 s sin recibirlo, se muestra «Panel sin conexión».
- La categoría se lee del campo Type solo para mostrarla: no cambia cómo se retransmite la trama.
- Por omisión se usa el controlador **SSD1306** (pantalla de 0.96"). Si la tuya es de 1.3" (SH1106) y la imagen se ve recorrida 2 píxeles, agrega `-DPANTALLA_SH1106` a `build_flags` en `platformio.ini`.
- Si la placa no tiene pantalla, el firmware funciona igual sin ella.
- La imagen de arriba se generó compilando `src/pantalla.cpp` en la PC con la misma biblioteca U8g2; no es una foto de la placa.

## Grabar la placa

Necesitas **PlatformIO**: la extensión de VS Code o `pip install platformio`.

1. Conecta el T-Beam por USB con un cable de datos. Si Windows no muestra un puerto `COMx`, instala el driver del chip USB (en la v1.2 suele ser CH9102).
2. **Conecta la antena antes de encender la placa.** Transmitir sin antena puede dañar el radio.
3. Desde esta carpeta, elige el modo (con `--upload-port COM5` si hay varias placas conectadas):
   ```
   pio run -e pasarela -t upload
   pio run -e modem -t upload
   pio device monitor
   ```
   La primera compilación descarga el compilador del ESP32 (varios cientos de MB).
4. En el monitor debe aparecer `# Nodo listo (…)`. En modo pasarela, cierra el monitor (Ctrl+C) antes de iniciar el puente: solo un programa puede usar el puerto a la vez.

Si aparece `ERROR: no se encontro el transceptor LoRa`, revisa que la placa sea una v1.2 con AXP2101. Las v1.0 y v1.1 usan AXP192 y necesitan otra inicialización de energía.

## Parámetros de radio

Están en `include/config.h`. **Todos los T-Beam de la red deben usar exactamente los mismos valores**: si uno difiere, no se escuchan.

| Parámetro | Valor | Nota |
|-----------|-------|------|
| Frecuencia | 915.0 MHz | Banda ISM 902–928 MHz |
| Ancho de banda | 125 kHz | |
| Factor de dispersión | SF9 | Una alerta de 56 B tarda unos 350 ms en el aire |
| Tasa de código | 4/5 | |
| Palabra de sincronía | privada (0x12) | Compatible entre SX1262 y SX1276 |
| Potencia | 17 dBm | Verificar la normatividad vigente antes de subirla |
| Retransmisión | TTL > 1, retardo de 100 a 600 ms | Igual en los dos modos |

Son una **propuesta inicial**: la SRS y el SDS aún no los fijan. Conviene confirmarlos con pruebas de alcance en el campus y registrarlos en el SDS.

## Probar sin teléfonos

Con dos T-Beam grabados como pasarela:

1. El primero, con el puente y el panel funcionando.
2. El segundo, conectado a otro puerto USB, con `pio device monitor --echo`. En el monitor pega esta línea, que es una alerta médica válida («Herido») generada con el codec del panel, y pulsa Enter:
   ```
   {"hex":"0BADCAFE1234ABCD05120D929430C2D1EBD06AF3FE9748657269646F0000000000000000000000000000000000000000000000000000BB00"}
   ```
3. El incidente debe aparecer en el tablero del panel, con el RSSI y el SNR del enlace.

Cada trama se acepta una sola vez: el panel y las pasarelas descartan un Msg_ID repetido. Para repetir la prueba usa otra trama, por ejemplo una alerta generada con el simulador de radio (`GET /enviadas` muestra el hexadecimal).
