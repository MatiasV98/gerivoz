#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

// ============================================================
//  ADPCM IMA de 4 bits — compresion 4:1
// ============================================================
// A 8 kHz da 4 bits x 8000 = 32 kbps, que es el mismo bitrate que usa
// G.726 en los telefonos inalambricos DECT. De ahi que suene "a telefono".
//
// Por que hace falta: el audio sin comprimir a 8 kHz y 16 bits son
// 128 kbps, y el CC1101 en esta configuracion da 99.98 kbps brutos
// (~89.7 kbps utiles descontando preambulo, sync, largo y CRC). O sea que
// PCM no entra en el canal ni con delay cero. Con 4:1 la ocupacion baja al
// ~53 % y recien ahi se puede mandar en tiempo real.
//
// Medido sobre una senial de prueba tipo voz: 25.5 dB de SNR.

struct AdpcmEstado
{
    int16_t pred  = 0;      // prediccion (la ultima muestra reconstruida)
    int8_t  index = 0;      // indice en la tabla de pasos, 0..88
};

// Codifica 'n' muestras (n PAR) en n/2 bytes, dos muestras por byte.
//
// Deja en '*inicial' el estado PREVIO a codificar. Ese estado es el que
// viaja en la cabecera del paquete, y es lo que hace que cada paquete se
// pueda decodificar solo: si se pierde uno, el error no se propaga a los
// que siguen (verificado: la SNR despues del hueco queda igual).
void adpcmCodificar(AdpcmEstado* st, const int16_t* src, int n,
                    uint8_t* dst, AdpcmEstado* inicial);

// Decodifica 'nBytes' bytes en 2*nBytes muestras, arrancando de '*st'.
void adpcmDecodificar(AdpcmEstado* st, const uint8_t* src, int nBytes,
                      int16_t* dst);

#endif // ADPCM_H
