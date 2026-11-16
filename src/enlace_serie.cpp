// Enlace del modo pasarela: puerto serie USB con el puente de radio
// (PanelCampus/puente_radio). Una línea JSON por mensaje:
//
//   T-Beam -> puente: {"hex":"0BADCAFE...","rssi":-102,"snr":-3.0}
//   puente -> T-Beam: {"hex":"..."}   trama por transmitir
//   puente -> T-Beam: {"ping":1}      latido mientras el panel está conectado
//
// Las líneas que el firmware imprime con '#' son informativas y el puente
// las ignora.

#include <Arduino.h>

#include "config.h"
#include "enlace.h"

namespace {

uint32_t ultimoLatido = 0;  // millis() del último {"ping"}; 0 = nunca

char lineaSerie[2 * MAX_TRAMA + 64];
size_t largoLinea = 0;
bool lineaDesbordada = false;

int valorHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void manejarLinea(const char *linea) {
  if (strstr(linea, "\"ping\"") != nullptr) {
    ultimoLatido = millis() | 1;  // | 1: 0 significa «nunca»
    return;
  }
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
  if (*p != '"' || transmitirTrama(datos, largo) == ResultadoEncolado::invalida) {
    Serial.println("# Linea del puente ilegible: se descarta");
  }
}

}  // namespace

void enlaceIniciar(const char *) {}

void enlaceProcesar() {
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

void enlaceEntregar(const uint8_t *b, size_t largo, float rssi, float snr) {
  Serial.print("{\"hex\":\"");
  for (size_t i = 0; i < largo; i++) Serial.printf("%02X", b[i]);
  Serial.printf("\",\"rssi\":%ld,\"snr\":%.1f}\n", lroundf(rssi), snr);
}

void enlaceInformarBateria(int, bool) {}  // la pasarela se alimenta por USB (RNF-6.6)

bool enlaceConectado() { return ultimoLatido != 0 && millis() - ultimoLatido < PANEL_SIN_LATIDO_MS; }
const char *enlaceModo() { return "PASARELA"; }
const char *enlaceTextoConectado() { return "Panel conectado"; }
const char *enlaceTextoDesconectado() { return "Panel sin conexión"; }
const char *enlaceNombre() { return nullptr; }
