#ifndef DSP_H
#define DSP_H

#include <stdint.h>

// ============================================================
//  FILTRADO Y CAMBIO DE FRECUENCIA DE MUESTREO
// ============================================================
// El I2S corre fijo a 16 kHz (SAMPLE_RATE) porque es donde el INMP441
// queda comodo de reloj. El codec trabaja a 8 kHz (CODEC_RATE), que es la
// frecuencia de muestreo de la telefonia: 4 kHz de banda util.
//
//   mic  -> 16 kHz -> FirDecimador   -> 8 kHz -> ADPCM -> RF
//   RF   -> ADPCM  -> 8 kHz -> FirInterpolador -> 16 kHz -> parlante
//
// El filtro es UNO solo (pasabajos de 31 taps, ventana de Hamming,
// fc = 3500 Hz a 16 kHz) usado en las dos direcciones. Coeficientes en
// Q15 (suma = 32768, o sea ganancia 1 en continua).
//
// Por que un FIR y no el promedio de 2 muestras que habia antes: ese
// promedio es un boxcar de 2 taps que atenua solo 3 dB en 4 kHz, asi que
// todo lo que entra entre 4 y 8 kHz se pliega sobre la banda util. Medido
// sobre estos coeficientes, la peor atenuacion en la banda que aliasea
// (4.2-8 kHz) pasa de -3.4 dB (boxcar) a -31.4 dB (este FIR).

#define FIR_N        31                 // taps del prototipo
#define FIR_HIST_N   16                 // ceil(FIR_N/2), potencia de 2
#define FIR_DEC_N    32                 // >= FIR_N, potencia de 2

extern const int16_t FIR_TAPS_Q15[FIR_N];

// Decimador 2:1 — consume muestras a 16 kHz, entrega a 8 kHz.
class FirDecimador
{
public:
    void reset();

    // Consume 'n' muestras de 'src' (16 kHz) y escribe en 'dst' (8 kHz).
    // 'dst' tiene que tener lugar para n/2 + 1. Devuelve cuantas escribio.
    int  procesar(const int16_t* src, int n, int16_t* dst);

private:
    int16_t hist[FIR_DEC_N] = {0};
    uint8_t pos  = 0;
    uint8_t fase = 0;                   // una salida cada 2 entradas
};

// Interpolador 1:2 — consume a 8 kHz, entrega a 16 kHz.
// Implementado como polifase: la fase par usa los taps pares del prototipo
// y la impar los impares, asi no se multiplica por los ceros insertados.
class FirInterpolador
{
public:
    void reset();

    // Consume 'n' muestras de 'src' (8 kHz) y escribe 2*n en 'dst' (16 kHz).
    // Devuelve cuantas escribio.
    int  procesar(const int16_t* src, int n, int16_t* dst);

private:
    int16_t hist[FIR_HIST_N] = {0};
    uint8_t pos = 0;
};

#endif // DSP_H
