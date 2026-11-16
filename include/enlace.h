// Enlace entre el T-Beam y quien lo usa:
//   * modo pasarela: puerto serie USB con el puente de radio del panel (enlace_serie.cpp);
//   * modo módem: servicio BLE con la app del teléfono (enlace_ble.cpp).
// main.cpp (radio, malla y pantalla) es el mismo en los dos modos.
#pragma once

#include <cstddef>
#include <cstdint>

enum class ResultadoEncolado : uint8_t { encolada = 0, invalida = 1, colaLlena = 2 };

// Implementado en main.cpp: pone en la cola de radio una trama que llegó por
// el enlace (del panel o del teléfono).
ResultadoEncolado transmitirTrama(const uint8_t *datos, size_t largo);

// Implementado por el enlace de cada modo.
void enlaceIniciar(const char *radio);
void enlaceProcesar();  // se llama en cada vuelta de loop()
void enlaceEntregar(const uint8_t *trama, size_t largo, float rssi, float snr);
void enlaceInformarBateria(int porcentaje, bool cargando);  // -1: sin batería

bool enlaceConectado();
const char *enlaceModo();              // «PASARELA» o «MÓDEM»
const char *enlaceTextoConectado();    // franja de estado de la pantalla
const char *enlaceTextoDesconectado();
const char *enlaceNombre();            // nombre visible por BLE, o nullptr
