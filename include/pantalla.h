// Pantalla OLED de 128x64 del T-Beam (SSD1306 por omisión; SH1106 con
// -DPANTALLA_SH1106). Si la placa no tiene pantalla, todas las funciones
// regresan sin hacer nada.
#pragma once

#include <cstdint>

struct UltimaTrama {
  bool hay = false;
  uint8_t tipo = 0;     // campo Type de la trama
  int rssi = 0;         // dBm
  float snr = 0;        // dB
  uint32_t momento = 0; // millis() al recibirla
};

struct EstadoPasarela {
  const char *radio = "";
  bool panelConectado = false;
  uint32_t recibidas = 0;
  uint32_t enviadas = 0;
  uint32_t retransmitidas = 0;
  UltimaTrama ultima;
};

// Devuelve true si encontró la pantalla en el bus I2C.
bool pantallaIniciar();

// Pantalla de mensaje (arranque o error): encabezado, título y una línea.
void pantallaMensaje(const char *titulo, const char *detalle);

// Pantalla principal de la pasarela.
void pantallaActualizar(const EstadoPasarela &estado);
