// Parámetros de la red LoRa.
//
// IMPORTANTE: todos los T-Beam de la red (módems y pasarela) deben usar
// exactamente los mismos valores de radio; si uno difiere, no se escuchan.
// La SRS y el SDS aún no fijan estos valores: son una propuesta inicial que
// debe confirmarse en las pruebas de alcance del campus.
#pragma once

#include <cstddef>
#include <cstdint>

// --- Radio -------------------------------------------------------------------
constexpr float FRECUENCIA_MHZ = 915.0f;   // banda ISM 902–928 MHz
constexpr float ANCHO_BANDA_KHZ = 125.0f;
constexpr uint8_t FACTOR_DISPERSION = 9;  // SF9: ~350 ms en el aire para una alerta de 56 B
constexpr uint8_t TASA_CODIGO = 5;        // 4/5
constexpr uint16_t PREAMBULO = 8;
// Potencia de salida. Verificar contra la normatividad vigente de la banda de
// 915 MHz en México antes de subirla (SRS 2.5, restricciones regulatorias).
constexpr int8_t POTENCIA_DBM = 17;

// --- Protocolo (SDS, sección 7) ------------------------------------------------
constexpr uint8_t OFFSET_TTL = 8;          // byte del TTL; está fuera del CRC
constexpr uint8_t OFFSET_TYPE = 9;
constexpr size_t LARGO_MINIMO = 10;        // Msg_ID + Sender_ID + TTL + Type
constexpr size_t MAX_TRAMA = 255;

// --- Inundación controlada -----------------------------------------------------
// Una trama se retransmite si llega con TTL > 1, con el TTL reducido en uno.
// Con TTL_MAX = 5 en el emisor, una trama recorre como máximo cuatro
// retransmisiones. Debe coincidir con la regla del firmware de los módems.
constexpr uint32_t RETARDO_MIN_MS = 100;   // retardo aleatorio antes de retransmitir
constexpr uint32_t RETARDO_MAX_MS = 600;
constexpr size_t MEMORIA_IDS = 128;        // Msg_ID recordados para descartar duplicados
constexpr uint32_t VENTANA_DUPLICADOS_MS = 10UL * 60 * 1000;
constexpr size_t COLA_SALIDA = 8;

// --- Puerto serie con el puente de radio ----------------------------------------
constexpr uint32_t BAUDIOS = 115200;
constexpr uint32_t INTERVALO_ESTADO_MS = 30000;
// El puente envía {"ping":1} cada 10 s mientras el panel está conectado a él.
// Sin latido durante este tiempo, la pantalla muestra «Panel sin conexión».
constexpr uint32_t PANEL_SIN_LATIDO_MS = 25000;

// --- Pantalla y batería --------------------------------------------------------------
constexpr uint32_t INTERVALO_PANTALLA_MS = 500;
constexpr uint32_t INTERVALO_BATERIA_MS = 10000;

// --- Modo módem: Bluetooth ------------------------------------------------------------
// 1: el teléfono debe emparejarse con el módem antes de usarlo (SRS 3.1.4,
// emparejamiento estándar con vinculación). Se deja en 0 para las primeras
// pruebas, porque el emparejamiento agrega un diálogo en el teléfono y es
// una fuente común de fallas; activarlo antes de los simulacros.
#define BLE_REQUIERE_CIFRADO 0
