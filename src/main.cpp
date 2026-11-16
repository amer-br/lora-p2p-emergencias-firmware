// Firmware del T-Beam v1.2 en modo pasarela.
//
// La pasarela es un nodo fijo de la red LoRa conectado por USB a la
// computadora del panel. Por el puerto serie intercambia una línea JSON por
// trama con el puente de radio (PanelCampus/puente_radio):
//
//   T-Beam -> puente: {"hex":"0BADCAFE...","rssi":-102,"snr":-3.0}
//   puente -> T-Beam: {"hex":"..."}
//
// Las demás líneas que imprime (empiezan con '#') son informativas y el
// puente las ignora.
//
// Como cualquier nodo, también retransmite las tramas que escucha
// (inundación controlada: descarte de duplicados por Msg_ID, TTL y retardo
// aleatorio). No interpreta el contenido de las tramas (SRS, supuesto S1):
// el CRC-16 del protocolo lo validan el panel y la app.
//
// El transceptor se detecta al arrancar: el T-Beam v1.2 se vende con SX1262
// o con SX1276 y ambos comparten los pines CS y RST.

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Wire.h>
#include <XPowersLib.h>

#include "config.h"

#ifndef MODO_PASARELA
#error "Por ahora solo existe el modo pasarela: compila con -DMODO_PASARELA (pio run -e pasarela)."
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
PhysicalLayer *radio = nullptr;
const char *nombreRadio = "ninguno";

volatile bool hayPaquete = false;
void IRAM_ATTR alRecibir() { hayPaquete = true; }

struct Contadores {
  uint32_t recibidas = 0, duplicadas = 0, errores = 0, retransmitidas = 0,
           desdePanel = 0, enviadas = 0, fallidas = 0, colaLlena = 0;
} cuenta;

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

void encolar(const uint8_t *datos, size_t largo, uint32_t retardoMs) {
  for (Pendiente &p : cola) {
    if (p.usada) continue;
    memcpy(p.datos, datos, largo);
    p.largo = largo;
    p.momento = millis() + retardoMs;
    p.usada = true;
    return;
  }
  cuenta.colaLlena++;
  Serial.println("# Cola de salida llena: se descarta una trama");
}

uint32_t retardoAleatorio() {
  return RETARDO_MIN_MS + esp_random() % (RETARDO_MAX_MS - RETARDO_MIN_MS);
}

// --- Energía y radio ---------------------------------------------------------------
bool iniciarEnergia() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_SDA, PIN_SCL)) return false;
  // ALDO2 alimenta el transceptor LoRa; ALDO3, el GNSS, que la pasarela no usa.
  pmu.setALDO2Voltage(3300);
  pmu.enableALDO2();
  pmu.disableALDO3();
  delay(20);
  return true;
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
void enviarAlPuente(const uint8_t *b, size_t largo, float rssi, float snr) {
  Serial.print("{\"hex\":\"");
  for (size_t i = 0; i < largo; i++) Serial.printf("%02X", b[i]);
  Serial.printf("\",\"rssi\":%ld,\"snr\":%.1f}\n", lroundf(rssi), snr);
}

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
  enviarAlPuente(b, largo, rssi, snr);

  if (b[OFFSET_TTL] > 1) {
    b[OFFSET_TTL]--;
    encolar(b, largo, retardoAleatorio());
    cuenta.retransmitidas++;
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

// --- Puerto serie (puente de radio) ----------------------------------------------------
int valorHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Acepta {"hex":"..."}; cualquier otro campo se ignora.
void manejarLinea(const char *linea) {
  const char *p = strstr(linea, "\"hex\"");
  if (p == nullptr) return;
  p = strchr(p + 5, ':');
  if (p == nullptr) return;
  p = strchr(p, '"');
  if (p == nullptr) return;
  p++;

  uint8_t datos[MAX_TRAMA];
  size_t largo = 0;
  while (largo < MAX_TRAMA) {
    const int alto = valorHex(p[0]);
    const int bajo = alto < 0 ? -1 : valorHex(p[1]);
    if (bajo < 0) break;
    datos[largo++] = uint8_t(alto << 4 | bajo);
    p += 2;
  }
  if (*p != '"' || largo < LARGO_MINIMO) {
    Serial.println("# Linea del puente ilegible: se descarta");
    return;
  }
  // La trama del panel se recuerda para no retransmitirla cuando vuelva.
  recordar(leerMsgId(datos));
  encolar(datos, largo, 0);
  cuenta.desdePanel++;
}

char lineaSerie[2 * MAX_TRAMA + 64];
size_t largoLinea = 0;
bool lineaDesbordada = false;

void procesarSerie() {
  while (Serial.available() > 0) {
    const char c = char(Serial.read());
    if (c == '\n') {
      lineaSerie[largoLinea] = '\0';
      if (!lineaDesbordada) manejarLinea(lineaSerie);
      largoLinea = 0;
      lineaDesbordada = false;
    } else if (c != '\r') {
      if (largoLinea < sizeof(lineaSerie) - 1) {
        lineaSerie[largoLinea++] = c;
      } else {
        lineaDesbordada = true;
      }
    }
  }
}

void informarEstado() {
  static uint32_t ultimo = 0;
  if (millis() - ultimo < INTERVALO_ESTADO_MS) return;
  ultimo = millis();
  Serial.printf("# Estado: radio=%s recibidas=%lu duplicadas=%lu errores=%lu retransmitidas=%lu "
                "del_panel=%lu enviadas=%lu fallidas=%lu cola_llena=%lu\n",
                nombreRadio, (unsigned long)cuenta.recibidas, (unsigned long)cuenta.duplicadas,
                (unsigned long)cuenta.errores, (unsigned long)cuenta.retransmitidas,
                (unsigned long)cuenta.desdePanel, (unsigned long)cuenta.enviadas,
                (unsigned long)cuenta.fallidas, (unsigned long)cuenta.colaLlena);
}

}  // namespace

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(BAUDIOS);
  delay(200);
  Serial.println("# Pasarela LoRa (T-Beam v1.2) iniciando");

  if (!iniciarEnergia()) {
    Serial.println("# ERROR: no se encontro el AXP2101; el radio podria quedar sin energia");
  }

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  if (!iniciarSX1262() && !iniciarSX1276()) {
    while (true) {
      Serial.println("# ERROR: no se encontro el transceptor LoRa (ni SX1262 ni SX1276)");
      delay(5000);
    }
  }

  radio->setPacketReceivedAction(alRecibir);
  escuchar();
  Serial.printf("# Pasarela lista: %s, %.1f MHz, SF%u, BW %.0f kHz, CR 4/%u, %d dBm\n", nombreRadio,
                FRECUENCIA_MHZ, FACTOR_DISPERSION, ANCHO_BANDA_KHZ, TASA_CODIGO, POTENCIA_DBM);
}

void loop() {
  procesarRecepcion();
  procesarSerie();
  procesarTransmision();
  informarEstado();
}
