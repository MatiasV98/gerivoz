#include "Dsp.h"

// Pasabajos de 31 taps, ventana de Hamming, fc = 3500 Hz a fs = 16 kHz.
// Simetrico (fase lineal) y con suma exactamente 32768 (ganancia 1 en
// continua). Generado con el script de diseno documentado en el README.
const int16_t FIR_TAPS_Q15[FIR_N] = {
        54,     26,    -80,   -103,    122,    298,    -90,   -640,
      -171,   1093,    891,  -1560,  -2632,   1913,  10108,  14310,
     10108,   1913,  -2632,  -1560,    891,   1093,   -171,   -640,
       -90,    298,    122,   -103,    -80,     26,     54,
};

static inline int16_t sat16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// ------------------------------------------------------------ decimador
void FirDecimador::reset()
{
    for (int i = 0; i < FIR_DEC_N; i++) hist[i] = 0;
    pos  = 0;
    fase = 0;
}

int FirDecimador::procesar(const int16_t* src, int n, int16_t* dst)
{
    int out = 0;

    for (int i = 0; i < n; i++)
    {
        pos = (uint8_t)((pos + 1) & (FIR_DEC_N - 1));
        hist[pos] = src[i];

        fase ^= 1;
        if (fase) continue;             // solo una salida cada dos entradas

        // La cota del acumulador es sum|h| * 32768 = 53872 * 32768 = 1.77e9,
        // dentro de int32 (2.15e9), asi que no hay desborde intermedio.
        int32_t acc = 0;
        for (int j = 0; j < FIR_N; j++) {
            acc += (int32_t)FIR_TAPS_Q15[j] * hist[(pos - j) & (FIR_DEC_N - 1)];
        }
        dst[out++] = sat16((acc + (1 << 14)) >> 15);
    }

    return out;
}

// ---------------------------------------------------------- interpolador
void FirInterpolador::reset()
{
    for (int i = 0; i < FIR_HIST_N; i++) hist[i] = 0;
    pos = 0;
}

int FirInterpolador::procesar(const int16_t* src, int n, int16_t* dst)
{
    int out = 0;

    for (int i = 0; i < n; i++)
    {
        pos = (uint8_t)((pos + 1) & (FIR_HIST_N - 1));
        hist[pos] = src[i];

        // Fase par: taps 0,2,4,...  Fase impar: taps 1,3,5,...
        // Cada fase suma ~16384, asi que el >>14 deja ganancia ~1.
        int32_t par = 0, impar = 0;
        for (int j = 0; j < FIR_HIST_N; j++) {
            int32_t x = hist[(pos - j) & (FIR_HIST_N - 1)];
            par += (int32_t)FIR_TAPS_Q15[2 * j] * x;
            if (2 * j + 1 < FIR_N) {
                impar += (int32_t)FIR_TAPS_Q15[2 * j + 1] * x;
            }
        }

        dst[out++] = sat16((par   + (1 << 13)) >> 14);
        dst[out++] = sat16((impar + (1 << 13)) >> 14);
    }

    return out;
}
