// Enlace del modo módem: servicio BLE GATT con la app del teléfono
// (SRS 3.1.2; SDS 8.7). La app y el módem deben usar los mismos UUID.
//
//   Servicio  8f1d0001-6b3a-4c2e-9e5d-2a7c4b9e1f00
//   TX_FRAME  8f1d0002-…  escritura con respuesta: la app escribe la trama
//                         binaria (alerta 56 B, acuse 20 B o Safety Check 17 B).
//   RX_FRAME  8f1d0003-…  notificación: trama recibida por radio, precedida de
//                         sus metadatos:
//                           [0..1] RSSI en dBm, int16 big-endian
//                           [2]    SNR en cuartos de dB, int8
//                           [3..]  trama
//   STATUS    8f1d0004-…  lectura y notificación, 9 bytes:
//                           [0]    versión del formato (1)
//                           [1]    banderas: bit0 radio lista, bit1 cargando
//                           [2]    batería en % (0xFF si no hay batería)
//                           [3..6] Msg_ID de la última trama escrita en TX_FRAME
//                           [7]    resultado de esa trama: 0 encolada,
//                                  1 inválida, 2 cola llena
//                           [8]    transceptor: 1 SX1262, 2 SX1276
//
// La app sabe que su trama quedó encolada cuando STATUS notifica su Msg_ID con
// resultado 0. Una trama de 56 B más los 3 B de metadatos no cabe en el MTU
// por omisión de BLE (20 B útiles): la app debe negociar un MTU mayor
// (flutter_blue_plus pide 512 en Android; iOS negocia uno suficiente).
//
// Solo se acepta un teléfono a la vez: al conectarse uno, el módem deja de
// anunciarse, y vuelve a hacerlo cuando se desconecta.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "config.h"
#include "enlace.h"

namespace {

const char *const UUID_SERVICIO = "8f1d0001-6b3a-4c2e-9e5d-2a7c4b9e1f00";
const char *const UUID_TX_FRAME = "8f1d0002-6b3a-4c2e-9e5d-2a7c4b9e1f00";
const char *const UUID_RX_FRAME = "8f1d0003-6b3a-4c2e-9e5d-2a7c4b9e1f00";
const char *const UUID_STATUS = "8f1d0004-6b3a-4c2e-9e5d-2a7c4b9e1f00";

constexpr uint8_t VERSION_STATUS = 1;
constexpr size_t LARGO_STATUS = 9;
constexpr size_t LARGO_METADATOS = 3;

// Tramas escritas por el teléfono. NimBLE las entrega en su propia tarea; se
// pasan a loop() por esta cola para no tocar el radio desde otra tarea.
struct TramaDelTelefono {
  uint16_t largo;  // 0: la escritura excedió MAX_TRAMA
  uint8_t datos[MAX_TRAMA];
};
QueueHandle_t entrantes = nullptr;
constexpr UBaseType_t CAPACIDAD_ENTRANTES = 8;

NimBLECharacteristic *rxFrame = nullptr;
NimBLECharacteristic *status = nullptr;
volatile bool conectado = false;
char nombre[16] = "LoRa";

uint8_t chip = 0;
int bateria = -1;
bool cargando = false;
uint32_t ultimoMsgId = 0;
ResultadoEncolado ultimoResultado = ResultadoEncolado::encolada;

void publicarStatus(bool notificar) {
  uint8_t s[LARGO_STATUS];
  s[0] = VERSION_STATUS;
  s[1] = 0x01 | (cargando ? 0x02 : 0x00);
  s[2] = bateria < 0 ? 0xFF : uint8_t(bateria);
  s[3] = uint8_t(ultimoMsgId >> 24);
  s[4] = uint8_t(ultimoMsgId >> 16);
  s[5] = uint8_t(ultimoMsgId >> 8);
  s[6] = uint8_t(ultimoMsgId);
  s[7] = uint8_t(ultimoResultado);
  s[8] = chip;
  status->setValue(s, sizeof s);
  if (notificar && conectado) status->notify();
}

class AlConectarse : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override {
    conectado = true;
    Serial.println("# Telefono conectado");
  }
  void onDisconnect(NimBLEServer *) override {
    conectado = false;
    Serial.println("# Telefono desconectado; el modem vuelve a anunciarse");
  }
};

class AlEscribirTrama : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    const NimBLEAttValue valor = c->getValue();
    TramaDelTelefono t;
    if (valor.length() > MAX_TRAMA) {
      t.largo = 0;
    } else {
      t.largo = uint16_t(valor.length());
      memcpy(t.datos, valor.data(), t.largo);
    }
    if (xQueueSend(entrantes, &t, 0) != pdTRUE) {
      Serial.println("# Cola BLE llena: se descarta una trama del telefono");
    }
  }
};

}  // namespace

void enlaceIniciar(const char *radio) {
  chip = strcmp(radio, "SX1262") == 0 ? 1 : strcmp(radio, "SX1276") == 0 ? 2 : 0;
  entrantes = xQueueCreate(CAPACIDAD_ENTRANTES, sizeof(TramaDelTelefono));

  // Nombre visible: «LoRa-» y los dos últimos bytes de la MAC, para
  // distinguir los módems en la lista del teléfono.
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(nombre, sizeof nombre, "LoRa-%02X%02X", uint8_t(mac >> 32), uint8_t(mac >> 40));

  NimBLEDevice::init(nombre);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
#if BLE_REQUIERE_CIFRADO
  // Emparejamiento estándar con vinculación (SRS 3.1.4).
  NimBLEDevice::setSecurityAuth(true, false, true);
  constexpr uint32_t ESCRITURA = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC;
  constexpr uint32_t LECTURA = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC;
#else
  constexpr uint32_t ESCRITURA = NIMBLE_PROPERTY::WRITE;
  constexpr uint32_t LECTURA = NIMBLE_PROPERTY::READ;
#endif

  NimBLEServer *servidor = NimBLEDevice::createServer();
  servidor->setCallbacks(new AlConectarse());
  NimBLEService *servicio = servidor->createService(UUID_SERVICIO);

  servicio->createCharacteristic(UUID_TX_FRAME, ESCRITURA)->setCallbacks(new AlEscribirTrama());
  rxFrame = servicio->createCharacteristic(UUID_RX_FRAME, NIMBLE_PROPERTY::NOTIFY);
  status = servicio->createCharacteristic(UUID_STATUS, LECTURA | NIMBLE_PROPERTY::NOTIFY);
  publicarStatus(false);
  servicio->start();

  NimBLEAdvertising *anuncio = NimBLEDevice::getAdvertising();
  anuncio->addServiceUUID(UUID_SERVICIO);
  anuncio->setScanResponse(true);  // el nombre va en la respuesta de escaneo
  anuncio->start();
  Serial.printf("# Modem BLE visible como %s\n", nombre);
}

void enlaceProcesar() {
  TramaDelTelefono t;
  while (xQueueReceive(entrantes, &t, 0) == pdTRUE) {
    ultimoMsgId = t.largo >= 4 ? (uint32_t(t.datos[0]) << 24) | (uint32_t(t.datos[1]) << 16) |
                                     (uint32_t(t.datos[2]) << 8) | t.datos[3]
                               : 0;
    ultimoResultado = t.largo == 0 ? ResultadoEncolado::invalida : transmitirTrama(t.datos, t.largo);
    publicarStatus(true);
  }
}

void enlaceEntregar(const uint8_t *trama, size_t largo, float rssi, float snr) {
  if (!conectado || largo > MAX_TRAMA) return;
  uint8_t b[MAX_TRAMA + LARGO_METADATOS];
  const int16_t r = int16_t(lroundf(rssi));
  const long cuartos = lroundf(snr * 4);
  b[0] = uint8_t(uint16_t(r) >> 8);
  b[1] = uint8_t(r);
  b[2] = uint8_t(int8_t(constrain(cuartos, -128L, 127L)));
  memcpy(b + LARGO_METADATOS, trama, largo);
  rxFrame->setValue(b, largo + LARGO_METADATOS);
  rxFrame->notify();
}

void enlaceInformarBateria(int porcentaje, bool estaCargando) {
  if (porcentaje == bateria && estaCargando == cargando) return;
  bateria = porcentaje;
  cargando = estaCargando;
  publicarStatus(true);
}

bool enlaceConectado() { return conectado; }
const char *enlaceModo() { return "MÓDEM"; }
const char *enlaceTextoConectado() { return "Teléfono conectado"; }
const char *enlaceTextoDesconectado() { return "Sin teléfono"; }
const char *enlaceNombre() { return nombre; }
