// Pantalla del nodo (pasarela o módem) con la identidad visual de la app móvil:
//
//   ┌────────────────────────────┐
//   │▓LoRa P2P▓▓▓▓▓▓▓▓▓PASARELA▓│  barra superior (en la app, la barra verde);
//   │                            │  en modo módem: «MÓDEM 87%»
//   │● Panel conectado           │  franja de estado (como «● Módem conectado»)
//   │╭──────────────────────────╮│
//   │ SOS               [ROJA]   │  tarjeta de la última trama, con el chip de
//   │ -87 dBm · 9.5 dB    12 s   │  prioridad de la app; se invierte con una
//   │╰──────────────────────────╯│  alerta roja reciente
//   │ Rx 12 │ Tx 3   │ Ret 5     │  barra inferior (en la app, la navegación)
//   └────────────────────────────┘

#include "pantalla.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"

namespace {

#ifdef PANTALLA_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif

constexpr uint8_t DIRECCION_OLED = 0x3C;
constexpr int ANCHO = 128;
// Una alerta de prioridad roja se resalta (tarjeta invertida) durante este tiempo.
constexpr uint32_t RESALTE_MS = 30000;

bool hayPantalla = false;

struct Categoria {
  const char *nombre;
  const char *chip;
  bool roja;
};

// Mismo catálogo que la app (lib/domain/categoria.dart) y el SDS (sección 8).
Categoria categoriaDe(uint8_t tipo) {
  switch (tipo) {
    case 0x01: return {"SOS", "ROJA", true};
    case 0x11: return {"Incendio", "ROJA", true};
    case 0x12: return {"Médica", "AMARILLA", false};
    case 0x13: return {"Estructural", "VERDE", false};
    case 0x02: return {"En camino", "ACUSE", false};
    case 0x03: return {"Llegué", "ACUSE", false};
    case 0x04: return {"Resuelto", "ACUSE", false};
    case 0x05: return {"A salvo", "CENSO", false};
    default: return {"Desconocida", "?", false};
  }
}

void textoCentrado(const char *texto, int base) {
  oled.drawUTF8((ANCHO - oled.getUTF8Width(texto)) / 2, base, texto);
}

void encabezado(const char *modo, int bateria) {
  oled.setDrawColor(1);
  oled.drawBox(0, 0, ANCHO, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_helvB08_tf);
  const int finTitulo = 3 + oled.drawUTF8(3, 10, "LoRa P2P");
  // Antena: mástil y ondas, separada del título.
  const int x = finTitulo + 6;
  oled.drawVLine(x, 4, 6);
  oled.drawPixel(x - 2, 4);
  oled.drawPixel(x + 2, 4);
  oled.drawPixel(x - 3, 2);
  oled.drawPixel(x - 3, 3);
  oled.drawPixel(x + 3, 2);
  oled.drawPixel(x + 3, 3);
  oled.setFont(u8g2_font_5x7_tf);
  char derecha[20];
  if (bateria >= 0) {
    snprintf(derecha, sizeof derecha, "%s %d%%", modo, bateria);
  } else {
    snprintf(derecha, sizeof derecha, "%s", modo);
  }
  oled.drawUTF8(ANCHO - 3 - oled.getUTF8Width(derecha), 9, derecha);
  oled.setDrawColor(1);
}

void franjaEstado(bool conectado, const char *texto) {
  oled.setFont(u8g2_font_6x10_tf);
  if (conectado) {
    oled.drawDisc(4, 19, 2);
  } else {
    oled.drawCircle(4, 19, 2);
  }
  oled.drawUTF8(10, 23, texto);
}

void formatearEdad(char *buf, size_t n, uint32_t ms) {
  const uint32_t s = ms / 1000;
  if (s < 60) {
    snprintf(buf, n, "%lu s", (unsigned long)s);
  } else if (s < 3600) {
    snprintf(buf, n, "%lu min", (unsigned long)(s / 60));
  } else {
    snprintf(buf, n, "%lu h", (unsigned long)(s / 3600));
  }
}

void tarjetaEsperando(const char *radio, const char *nombre) {
  oled.drawRFrame(0, 27, ANCHO, 26, 3);
  oled.setFont(u8g2_font_6x10_tf);
  textoCentrado("Escuchando la red", 39);
  char linea[40];
  if (nombre != nullptr) {
    // Módem: el nombre con el que el teléfono lo encuentra por Bluetooth.
    snprintf(linea, sizeof linea, "%s · %s · SF%u", nombre, radio, FACTOR_DISPERSION);
  } else {
    snprintf(linea, sizeof linea, "%s · %.0f MHz · SF%u", radio, FRECUENCIA_MHZ, FACTOR_DISPERSION);
  }
  oled.setFont(u8g2_font_5x7_tf);
  textoCentrado(linea, 49);
}

void tarjetaTrama(const UltimaTrama &t) {
  const Categoria c = categoriaDe(t.tipo);
  const uint32_t edad = millis() - t.momento;
  // Fondo de la tarjeta: 1 (invertida) para una alerta roja reciente.
  const uint8_t fondo = (c.roja && edad < RESALTE_MS) ? 1 : 0;

  if (fondo) {
    oled.drawRBox(0, 27, ANCHO, 26, 3);
  } else {
    oled.drawRFrame(0, 27, ANCHO, 26, 3);
  }

  // Chip de prioridad, siempre en el color opuesto a la tarjeta.
  oled.setFont(u8g2_font_5x7_tf);
  const int anchoChip = oled.getUTF8Width(c.chip) + 6;
  const int xChip = ANCHO - 4 - anchoChip;
  oled.setDrawColor(!fondo);
  oled.drawRBox(xChip, 30, anchoChip, 10, 2);
  oled.setDrawColor(fondo);
  oled.drawUTF8(xChip + 3, 38, c.chip);

  oled.setDrawColor(!fondo);
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawUTF8(4, 40, c.nombre);

  char hace[12];
  formatearEdad(hace, sizeof hace, edad);
  oled.setFont(u8g2_font_5x7_tf);
  const int anchoHace = oled.getUTF8Width(hace);
  // Con valores largos (p. ej. -104 dBm y -3.2 dB) se quita el separador
  // para que el enlace no choque con la antigüedad.
  char enlace[32];
  snprintf(enlace, sizeof enlace, "%d dBm · %.1f dB", t.rssi, t.snr);
  if (4 + oled.getUTF8Width(enlace) + 10 > ANCHO - 4 - anchoHace) {
    snprintf(enlace, sizeof enlace, "%d dBm %.1f dB", t.rssi, t.snr);
  }
  oled.drawUTF8(4, 50, enlace);
  oled.drawUTF8(ANCHO - 4 - anchoHace, 50, hace);
  oled.setDrawColor(1);
}

void barraInferior(const EstadoPantalla &e) {
  oled.drawHLine(0, 55, ANCHO);
  oled.drawVLine(42, 56, 8);
  oled.drawVLine(85, 56, 8);
  oled.setFont(u8g2_font_5x7_tf);
  char celda[16];
  const struct {
    const char *etiqueta;
    uint32_t valor;
    int x;
  } celdas[] = {{"Rx", e.recibidas, 0}, {"Tx", e.enviadas, 43}, {"Ret", e.retransmitidas, 86}};
  for (const auto &c : celdas) {
    snprintf(celda, sizeof celda, "%s %lu", c.etiqueta, (unsigned long)c.valor);
    oled.drawUTF8(c.x + (42 - oled.getUTF8Width(celda)) / 2, 63, celda);
  }
}

}  // namespace

bool pantallaIniciar() {
  // La pantalla comparte el bus I2C con el AXP2101 (SDA 21, SCL 22).
  Wire.beginTransmission(DIRECCION_OLED);
  if (Wire.endTransmission() != 0) return false;
  oled.setBusClock(400000);
  hayPantalla = oled.begin();
  if (hayPantalla) oled.setFontMode(1);  // texto transparente sobre los fondos
  return hayPantalla;
}

void pantallaMensaje(const char *modo, const char *titulo, const char *detalle) {
  if (!hayPantalla) return;
  oled.clearBuffer();
  encabezado(modo, -1);
  oled.setFont(u8g2_font_7x13B_tf);
  textoCentrado(titulo, 36);
  oled.setFont(u8g2_font_5x7_tf);
  textoCentrado(detalle, 50);
  oled.sendBuffer();
}

void pantallaActualizar(const EstadoPantalla &e) {
  if (!hayPantalla) return;
  oled.clearBuffer();
  encabezado(e.modo, e.bateria);
  franjaEstado(e.conectado, e.textoEnlace);
  if (e.ultima.hay) {
    tarjetaTrama(e.ultima);
  } else {
    tarjetaEsperando(e.radio, e.nombre);
  }
  barraInferior(e);
  oled.sendBuffer();
}
