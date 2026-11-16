// Firmware del T-Beam v1.2 (AXP2101) para la red LoRa de emergencias.
//
// Hay dos modos, que comparten este archivo (radio, malla y pantalla) y solo
// cambian el enlace (ver enlace.h):
//   * pasarela (pio run -e pasarela): nodo fijo conectado por USB al puente
//     de radio del panel (enlace_serie.cpp);
//   * módem (pio run -e modem): nodo de bolsillo conectado por BLE a la app
//     del teléfono (enlace_ble.cpp).
//
// En los dos modos el T-Beam retransmite lo que escucha (inundación
// controlada: descarte de duplicados por Msg_ID, TTL y retardo aleatorio).
// No interpreta el contenido de las tramas (SRS, supuesto S1): el CRC-16 del
// protocolo lo validan el panel y la app.
//
// El transceptor se detecta al arrancar: el T-Beam v1.2 se vende con SX1262
// o con SX1276 y ambos comparten los pines CS y RST.

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Wire.h>
#include <XPowersLib.h>

#include "config.h"
#include "enlace.h"
#include "pantalla.h"

#if !defined(MODO_PASARELA) && !defined(MODO_MODEM)
#error "Compila con pio run -e pasarela o pio run -e modem."
#endif

namespace {

// --- Pines del LilyGO T-Beam v1.2 ----------------------------------------------
constexpr int PIN_SDA = 21;
constexpr int PIN_SCL = 22;
constexpr int PIN_LORA_SCK = 5;
constexpr int PIN_LORA_MISO = 19;
constexpr int PIN_LORA_MOSI = 27;
constexpr int PIN_LORA_CS = 18;
constexpr int PIN_LORA_RST = 23;
constexpr int SX1262_DIO1 = 33;
constexpr int SX1262_BUSY = 32;
constexpr float SX1262_TCXO_V = 1.8f;
constexpr int SX1276_DIO0 = 26;
constexpr int SX1276_DIO1 = 33;

XPowersAXP2101 pmu;
bool hayPmu = false;
PhysicalLayer *radio = nullptr;
const char *nombreRadio = "ninguno";

volatile bool hayPaquete = false;
void IRAM_ATTR alRecibir() { hayPaquete = true; }

struct Contadores {
  uint32_t recibidas = 0, duplicadas = 0, errores = 0, retransmitidas = 0,
           delEnlace = 0, enviadas = 0, fallidas = 0, colaLlena = 0;
} cuenta;

UltimaTrama ultimaTrama;
int bateria = -1;  // % o -1 sin batería
bool cargando = false;

// --- Memoria de Msg_ID ya vistos -------------------------------------------------
struct Visto {
  uint32_t id;
  uint32_t momento;
};
Visto vistos[MEMORIA_IDS] = {};
size_t siguienteVisto = 0;

uint32_t leerMsgId(const uint8_t *b) {
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}

bool yaVisto(uint32_t id) {
  const uint32_t ahora = millis();
  for (const Visto &v : vistos) {
    if (v.momento != 0 && v.id == id && ahora - v.momento < VENTANA_DUPLICADOS_MS) return true;
  }
  return false;
}

void recordar(uint32_t id) {
  vistos[siguienteVisto] = {id, millis() | 1};  // | 1: 0 marca una entrada vacía
  siguienteVisto = (siguienteVisto + 1) % MEMORIA_IDS;
}

// --- Cola de salida --------------------------------------------------------------
struct Pendiente {
  bool usada = false;
  uint32_t momento = 0;
  size_t largo = 0;
  uint8_t datos[MAX_TRAMA];
};
Pendiente cola[COLA_SALIDA];

bool encolar(const uint8_t *datos, size_t largo, uint32_t retardoMs) {
  for (Pendiente &p : cola) {
    if (p.usada) continue;
    memcpy(p.datos, datos, largo);
    p.largo = largo;
    p.momento = millis() + retardoMs;
    p.usada = true;
    return true;
  }
  cuenta.colaLlena++;
  Serial.println("# Cola de salida llena: se descarta una trama");
  return false;
}

uint32_t retardoAleatorio() {
  return RETARDO_MIN_MS + esp_random() % (RETARDO_MAX_MS - RETARDO_MIN_MS);
}

// --- Energía y radio ---------------------------------------------------------------
bool iniciarEnergia() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_SDA, PIN_SCL)) return false;
  // ALDO2 alimenta el transceptor LoRa; ALDO3, el GNSS, que no se usa (la app
  // toma la posición del teléfono).
  pmu.setALDO2Voltage(3300);
  pmu.enableALDO2();
  pmu.disableALDO3();
  // El T-Beam v1.2 no tiene termistor en la batería: sin esto, el AXP2101
  // puede impedir la carga.
  pmu.disableTSPinMeasure();
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  delay(20);
  return true;
}

void leerBateria() {
  static uint32_t ultimo = 0;
  if (!hayPmu || (ultimo != 0 && millis() - ultimo < INTERVALO_BATERIA_MS)) return;
  ultimo = millis() | 1;
  bateria = pmu.isBatteryConnect() ? constrain(int(pmu.getBatteryPercent()), 0, 100) : -1;
  cargando = pmu.isCharging();
  enlaceInformarBateria(bateria, cargando);
}

bool iniciarSX1262() {
  auto *chip = new SX1262(new Module(PIN_LORA_CS, SX1262_DIO1, PIN_LORA_RST, SX1262_BUSY));
  int16_t e = chip->begin(FRECUENCIA_MHZ, ANCHO_BANDA_KHZ, FACTOR_DISPERSION, TASA_CODIGO,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, POTENCIA_DBM, PREAMBULO, SX1262_TCXO_V);
  if (e != RADIOLIB_ERR_NONE) {
    Serial.printf("# SX1262 no responde (codigo %d)\n", e);
    return false;  // el objeto se pierde a propósito: solo ocurre una vez al arrancar
  }
  chip->setDio2AsRfSwitch(true);
  chip->setCurrentLimit(140);
  chip->setCRC(2);
  radio = chip;
  nombreRadio = "SX1262";
  return true;
}

bool iniciarSX1276() {
  auto *chip = new SX1276(new Module(PIN_LORA_CS, SX1276_DIO0, PIN_LORA_RST, SX1276_DIO1));
  int16_t e = chip->begin(FRECUENCIA_MHZ, ANCHO_BANDA_KHZ, FACTOR_DISPERSION, TASA_CODIGO,
                          RADIOLIB_SX127X_SYNC_WORD, POTENCIA_DBM, PREAMBULO);
  if (e != RADIOLIB_ERR_NONE) {
    Serial.printf("# SX1276 no responde (codigo %d)\n", e);
    return false;
  }
  chip->setCRC(true);
  radio = chip;
  nombreRadio = "SX1276";
  return true;
}

void escuchar() {
  hayPaquete = false;
  int16_t e = radio->startReceive();
  if (e != RADIOLIB_ERR_NONE) Serial.printf("# No se pudo volver a recepcion (codigo %d)\n", e);
}

// --- Recepción por radio -------------------------------------------------------------
void procesarRecepcion() {
  if (!hayPaquete) return;
  hayPaquete = false;

  uint8_t b[MAX_TRAMA];
  const size_t largo = radio->getPacketLength();
  if (largo == 0 || largo > MAX_TRAMA) {
    escuchar();
    return;
  }
  const int16_t e = radio->readData(b, largo);
  const float rssi = radio->getRSSI();
  const float snr = radio->getSNR();
  escuchar();

  if (e != RADIOLIB_ERR_NONE || largo < LARGO_MINIMO) {
    cuenta.errores++;  // CRC de LoRa incorrecto o trama demasiado corta
    return;
  }
  const uint32_t id = leerMsgId(b);
  if (yaVisto(id)) {
    cuenta.duplicadas++;
    return;
  }
  recordar(id);
  cuenta.recibidas++;
  enlaceEntregar(b, largo, rssi, snr);

  ultimaTrama.hay = true;
  ultimaTrama.tipo = b[OFFSET_TYPE];
  ultimaTrama.rssi = int(lroundf(rssi));
  ultimaTrama.snr = snr;
  ultimaTrama.momento = millis();

  if (b[OFFSET_TTL] > 1) {
    b[OFFSET_TTL]--;
    if (encolar(b, largo, retardoAleatorio())) cuenta.retransmitidas++;
  }
}

// --- Transmisión -----------------------------------------------------------------------
void transmitir(Pendiente &p) {
  // Escucha antes de hablar: si alguien transmite, espera un poco (hasta 3 veces).
  for (int intento = 0; intento < 3; intento++) {
    if (radio->scanChannel() != RADIOLIB_LORA_DETECTED) break;
    delay(50 + esp_random() % 150);
  }
  const int16_t e = radio->transmit(p.datos, p.largo);
  if (e == RADIOLIB_ERR_NONE) {
    cuenta.enviadas++;
  } else {
    cuenta.fallidas++;
    Serial.printf("# Fallo al transmitir (codigo %d)\n", e);
  }
  p.usada = false;
  escuchar();
}

void procesarTransmision() {
  const uint32_t ahora = millis();
  for (Pendiente &p : cola) {
    if (p.usada && int32_t(ahora - p.momento) >= 0) {
      transmitir(p);
      return;  // una por vuelta, para no dejar de atender la recepción
    }
  }
}

// --- Estado ----------------------------------------------------------------------------
void informarEstado() {
  static uint32_t ultimo = 0;
  if (millis() - ultimo < INTERVALO_ESTADO_MS) return;
  ultimo = millis();
  Serial.printf("# Estado: radio=%s recibidas=%lu duplicadas=%lu errores=%lu retransmitidas=%lu "
                "del_enlace=%lu enviadas=%lu fallidas=%lu cola_llena=%lu bateria=%d\n",
                nombreRadio, (unsigned long)cuenta.recibidas, (unsigned long)cuenta.duplicadas,
                (unsigned long)cuenta.errores, (unsigned long)cuenta.retransmitidas,
                (unsigned long)cuenta.delEnlace, (unsigned long)cuenta.enviadas,
                (unsigned long)cuenta.fallidas, (unsigned long)cuenta.colaLlena, bateria);
}

void refrescarPantalla() {
  static uint32_t ultimo = 0;
  if (millis() - ultimo < INTERVALO_PANTALLA_MS) return;
  ultimo = millis();
  EstadoPantalla e;
  e.modo = enlaceModo();
  e.conectado = enlaceConectado();
  e.textoEnlace = e.conectado ? enlaceTextoConectado() : enlaceTextoDesconectado();
#ifdef MODO_MODEM
  e.bateria = bateria;  // la pasarela va por USB: no se muestra
#endif
  e.radio = nombreRadio;
  e.nombre = enlaceNombre();
  e.recibidas = cuenta.recibidas;
  e.enviadas = cuenta.enviadas;
  e.retransmitidas = cuenta.retransmitidas;
  e.ultima = ultimaTrama;
  pantallaActualizar(e);
}

}  // namespace

// Trama que llega del panel (pasarela) o del teléfono (módem).
ResultadoEncolado transmitirTrama(const uint8_t *datos, size_t largo) {
  if (largo < LARGO_MINIMO || largo > MAX_TRAMA) return ResultadoEncolado::invalida;
  // Se recuerda para no retransmitirla cuando vuelva por la red.
  recordar(leerMsgId(datos));
  if (!encolar(datos, largo, 0)) return ResultadoEncolado::colaLlena;
  cuenta.delEnlace++;
  return ResultadoEncolado::encolada;
}

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(BAUDIOS);
  delay(200);
  Serial.printf("# Nodo LoRa (T-Beam v1.2) iniciando en modo %s\n", enlaceModo());

  hayPmu = iniciarEnergia();
  if (!hayPmu) {
    Serial.println("# ERROR: no se encontro el AXP2101; el radio podria quedar sin energia");
  }
  if (pantallaIniciar()) {
    pantallaMensaje(enlaceModo(), "Iniciando", "Buscando el radio LoRa");
  } else {
    Serial.println("# Sin pantalla OLED: se continua sin ella");
  }

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  if (!iniciarSX1262() && !iniciarSX1276()) {
    pantallaMensaje(enlaceModo(), "Sin radio LoRa", "Revisa la placa y la antena");
    while (true) {
      Serial.println("# ERROR: no se encontro el transceptor LoRa (ni SX1262 ni SX1276)");
      delay(5000);
    }
  }

  enlaceIniciar(nombreRadio);
  leerBateria();
  radio->setPacketReceivedAction(alRecibir);
  escuchar();
  Serial.printf("# Nodo listo (%s): %s, %.1f MHz, SF%u, BW %.0f kHz, CR 4/%u, %d dBm\n", enlaceModo(),
                nombreRadio, FRECUENCIA_MHZ, FACTOR_DISPERSION, ANCHO_BANDA_KHZ, TASA_CODIGO,
                POTENCIA_DBM);
}

void loop() {
  procesarRecepcion();
  enlaceProcesar();
  procesarTransmision();
  leerBateria();
  informarEstado();
  refrescarPantalla();
}
