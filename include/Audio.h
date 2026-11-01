#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include "Config.h"

// ============================================================
//  I2S — mic (INMP441) y parlante en el mismo puerto
// ============================================================
// I2S_NUM_0 se usa para las dos cosas con una sola config, asi que el
// sistema es half-duplex: o se graba o se reproduce, igual que el CC1101.
//
// Las primitivas de streaming (leerMic / escribirParlante) NO BLOQUEAN por
// defecto: devuelven lo que el DMA tenia listo. Eso es lo que permite
// atender la radio y el audio en el mismo lazo sin perder paquetes.
//
// Todo lo que entra y sale de aca esta a SAMPLE_RATE (16 kHz). La conversion
// a los 8 kHz del codec la hace el FIR de Dsp.h, afuera de esta clase.
class Audio
{
public:
    bool begin();

    // Deja el puerto listo para grabar y descarta el audio viejo que quedo
    // encolado en el DMA de entrada mientras se reproducia.
    void modoGrabar();

    // Deja el puerto listo para reproducir (limpia el DMA de salida).
    void modoReproducir();

    // Lee hasta 'maxN' muestras a 16 kHz, ya convertidas a int16 con
    // saturacion. Devuelve cuantas leyo (puede ser 0).
    // 'ticksEspera' en ticks de FreeRTOS: 0 = no bloquea.
    int  leerMic(int16_t* dst, int maxN, int ticksEspera = 0);

    // Escribe hasta 'n' muestras a 16 kHz. Devuelve cuantas ENTRARON en el
    // DMA (puede ser menos que 'n', o 0 si esta lleno): el que llama tiene
    // que avanzar solo lo que devolvio.
    int  escribirParlante(const int16_t* src, int n);


    void silencio();

    // Cambia la frecuencia de muestreo del puerto. Solo para diagnostico:
    // cambia el BCLK, que es lo que el amplificador necesita para engancharse.
    // NO usar durante streaming (reconfigurar el reloj hace perder paquetes).
    void cambiarFrecuencia(uint32_t fs);

    // Buffer del monitor local ('m'), a CODEC_RATE.
    int16_t* monitor()                  { return monBuf; }
    int      monitorMuestras() const    { return MONITOR_MUESTRAS; }

private:
    int16_t* monBuf = nullptr;
};

#endif // AUDIO_H
