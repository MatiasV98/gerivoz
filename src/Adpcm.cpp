#include "Adpcm.h"

// Tablas estandar de IMA ADPCM.
static const int16_t PASOS[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t SALTO_INDEX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

// El paso que COMPARTEN codificador y decodificador. Que sea una sola
// funcion es lo que garantiza que el predictor del que transmite siga
// exactamente al del que recibe; si se escribieran por separado, la menor
// diferencia hace que deriven y el audio se degrade con el tiempo.
static inline void adpcmPaso(uint8_t code, int16_t* pred, int8_t* index)
{
    int32_t paso  = PASOS[*index];
    int32_t diffq = paso >> 3;

    if (code & 4) diffq += paso;
    if (code & 2) diffq += paso >> 1;
    if (code & 1) diffq += paso >> 2;

    int32_t p = (code & 8) ? (int32_t)*pred - diffq
                           : (int32_t)*pred + diffq;
    if (p >  32767) p =  32767;
    if (p < -32768) p = -32768;
    *pred = (int16_t)p;

    int idx = *index + SALTO_INDEX[code];
    if (idx <  0) idx =  0;
    if (idx > 88) idx = 88;
    *index = (int8_t)idx;
}

static inline uint8_t adpcmCodificarMuestra(int16_t muestra, int16_t* pred,
                                            int8_t* index)
{
    int32_t paso = PASOS[*index];
    int32_t diff = (int32_t)muestra - (int32_t)*pred;

    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }

    int32_t tmp = paso;
    if (diff >= tmp) { code |= 4; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 2; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 1; }

    // Avanzar con el MISMO paso que usa el decodificador.
    adpcmPaso(code, pred, index);
    return code;
}

void adpcmCodificar(AdpcmEstado* st, const int16_t* src, int n,
                    uint8_t* dst, AdpcmEstado* inicial)
{
    if (inicial) *inicial = *st;

    for (int i = 0; i < n; i += 2) {
        uint8_t lo = adpcmCodificarMuestra(src[i],     &st->pred, &st->index);
        uint8_t hi = adpcmCodificarMuestra(src[i + 1], &st->pred, &st->index);
        dst[i / 2] = (uint8_t)((hi << 4) | lo);
    }
}

void adpcmDecodificar(AdpcmEstado* st, const uint8_t* src, int nBytes,
                      int16_t* dst)
{
    for (int i = 0; i < nBytes; i++) {
        uint8_t b = src[i];

        adpcmPaso((uint8_t)(b & 0x0F), &st->pred, &st->index);
        dst[i * 2]     = st->pred;

        adpcmPaso((uint8_t)((b >> 4) & 0x0F), &st->pred, &st->index);
        dst[i * 2 + 1] = st->pred;
    }
}
