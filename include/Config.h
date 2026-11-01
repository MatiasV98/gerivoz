#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  PRUEBA DE ALCANCE CC1101 — pinout
//  Firmware simetrico: las dos placas se cablean IGUAL.
// ============================================================

//====================
// I2S compartido
//====================

#define I2S_BCLK   26
#define I2S_WS     25

#define MIC_DATA   32
#define SPK_DATA   22

// ============================================================
//  AUDIO — frecuencias de muestreo
// ============================================================
// El I2S corre FIJO a SAMPLE_RATE (16 kHz): ahi el INMP441 queda comodo de
// reloj (BCLK = 16000 * 32 * 2 = 1.024 MHz, bien dentro de su rango).
//
// El codec trabaja a CODEC_RATE (8 kHz) = 4 kHz de banda util, la misma que
// la telefonia. El cambio entre las dos frecuencias lo hace el FIR de
// Dsp.h, en las dos direcciones:
//
//   mic -> 16 kHz -> FirDecimador    -> 8 kHz -> ADPCM -> RF
//   RF  -> ADPCM  ->  8 kHz -> FirInterpolador -> 16 kHz -> parlante
//
// Antes se decimaba promediando 2 muestras, que es un boxcar y atenua solo
// 3 dB en 4 kHz: todo lo que habia entre 4 y 8 kHz se plegaba sobre la
// banda util. El FIR lleva esa peor atenuacion de -3.4 dB a -31.4 dB.
#define SAMPLE_RATE      16000
#define CODEC_RATE        8000
#define DECIMACION           2      // SAMPLE_RATE / CODEC_RATE (fijo: el FIR es 2:1)

// DMA del I2S: 4 buffers de 128 muestras = 32 ms. Antes eran 8 x 256 =
// 128 ms, que con streaming es latencia pura en el parlante.
#define I2S_DMA_BUFS         4
#define I2S_DMA_LEN        128

// ---------- Ganancia de captura del microfono ----------
// El INMP441 entrega 24 bits alineados al MSB dentro de una trama de 32.
// Con MIC_SHIFT = 16 se toman los 16 bits de arriba: ganancia 1 (0 dB).
//
// El problema es que ahi queda MUY bajo. El INMP441 tiene su fondo de escala
// en 120 dBSPL y la voz normal a 30 cm anda por los 62 dBSPL, o sea unos
// -58 dBFS: amplitud ~41 sobre 32768, apenas 5 bits utiles.
//
// Cada unidad MENOS de shift son +6 dB:
//   16 -> 0 dB | 14 -> +12 dB | 12 -> +24 dB | 10 -> +36 dB
//
// La conversion satura, asi que pasarse NO envuelve: recorta. Si se escucha
// distorsion en los picos fuertes, subir el numero; si sigue bajo, bajarlo.
// Usar 'm' para medir: imprime el nivel de pico y RMS en dBFS.
#define MIC_SHIFT   12

// En que slot del I2S habla el microfono. El INMP441 con L/R a GND usa uno
// solo de los dos; si 'm' informa pico=0 con el mic sano, cambiar a 1.
#define MIC_SLOT    1

// Buffer del monitor local ('m'): 1 s a 8 kHz = 16 KB. Antes habia uno de
// 48000 muestras (96 KB) porque se grababa todo antes de mandar; con
// streaming ya no hace falta.
#define MONITOR_SEGUNDOS     1
#define MONITOR_MUESTRAS  (CODEC_RATE * MONITOR_SEGUNDOS)

// ---------- CC1101 (SPI / VSPI) ----------
// Mismos pines que el proyecto de voz, para no recablear nada.
#define PIN_SCK        18
#define PIN_MISO       19
#define PIN_MOSI       23
#define PIN_CSN         5
#define PIN_GDO0        2
// GDO2 sin conectar

// ---------- Interfaz ----------
// Pulsador de PTT. Va entre el pin y 3V3: el firmware usa INPUT_PULLDOWN y
// dispara en ALTO (apretado = 3V3, suelto = lo baja la resistencia interna).
//
// CUIDADO, GPIO12 ES PIN DE STRAPPING. En el ESP32 el GPIO12 (MTDI) elige la
// tension de la flash al arrancar: en BAJO = 3.3 V (lo correcto), en ALTO =
// 1.8 V y la placa NO BOOTEA. Como el pulsador lo lleva a 3V3, hay una sola
// regla: NO ARRANCAR LA PLACA CON EL PULSADOR APRETADO. Si quedo trabado o
// se enchufa el USB con el boton puesto, parece que se colgo; se suelta y se
// resetea y arranca normal. Suelto no molesta, porque la resistencia interna
// lo mantiene en bajo.
#define BTN_PTT        12
// ============================================================
//  SEGUNDO PULSADOR: ALERTA
// ============================================================
//  >>> CAMBIAR ESTE NUMERO por el GPIO donde quede cableado. <<<
//
// Se cablea IGUAL que el PTT: del pin a 3V3. El firmware usa INPUT_PULLDOWN y
// dispara en alto, asi que suelto queda en bajo solo.
//
// Pines ya ocupados (no usar): 2, 5, 12, 14, 18, 19, 22, 23, 25, 26, 27, 32, 33.
// Evitar ademas los de strapping: 0 y 15 (el 12 ya se usa y por eso no hay que
// arrancar con el PTT apretado). Libres y seguros: 4, 13, 16, 17, 21.
#define BTN_ALERTA      4

// ---------- Sonido de la alarma ----------
// La alarma la genera el firmware del que RECIBE: por el aire viaja solo el
// aviso de 5 bytes, nunca el audio. Por eso la alerta llega mucho mas lejos
// que la voz, y por eso no consume ancho de banda.
//
// Es un bitono tipo sirena: alterna entre dos frecuencias. Corta mucho mejor
// el ruido ambiente que un tono fijo y no se confunde con el bip del PTT.
#define ALARMA_MS         2000     // duracion total
#define ALARMA_TRAMO_MS    250     // cada cuanto cambia de tono
#define ALARMA_HZ_A        800
#define ALARMA_HZ_B       1200
#define ALARMA_AMPL      14000     // ~-7.4 dBFS: fuerte pero sin recortar

#define LED_TX         27     // LED: se enciende al apretar el pulsador / transmitir
#define LED_RX         14     // LED: parpadea al recibir un paquete
#define PIN_BUZZER     33     // buzzer (via transistor NPN en la placa del TMB12A05)

// ============================================================
//  TIPO DE BUZZER  <-- IMPORTANTE, LEER
// ============================================================
// Test para saber cual tenes (sin programar): con VCC y GND puestos,
// tocá el pin de señal (S) a 3V3.
//    suena TONO CONTINUO  -> ACTIVO  (tiene oscilador propio)
//    hace un CLIC y nada   -> PASIVO  (hay que darle el tono nosotros)
//
//   1 = PASIVO  -> genera el tono por PWM (LEDC) a BUZZ_FREQ_HZ
//   0 = ACTIVO  -> solo prende/apaga con digitalWrite (ignora BUZZ_FREQ_HZ)
#define BUZZER_PASIVO   1

// ============================================================
//  RF
// ============================================================
#define RF_FREQ_MHZ    433.92
// Potencia de transmision.
//
// CUIDADO AL PROBAR EN LA MESA: con las dos placas juntas, 10 dBm SATURA el
// frente de RF del receptor y no se recibe NADA. Medido: a 0 dBm o menos
// entran todos los paquetes, a 5 dBm o mas no entra ninguno. El sintoma
// engania porque el RSSI muestra senial fuerte (-23 dBm) pero el chip nunca
// detecta el sync word, y falla en las dos direcciones.
//
// Para las pruebas de alcance hay que dejarlo en 10 (es el requisito de los
// 30 m). Para probar en el banco, bajarlo en caliente con la tecla 'w', que
// cicla -30/-20/-10/0/5/7/10 dBm sin recompilar. Al reiniciar vuelve a este
// valor.
#define RF_PA_DBM      10

// ============================================================
//  TONO DEL BUZZER (solo aplica si BUZZER_PASIVO = 1)
// ============================================================
// El TMB12A05 tiene su frecuencia de resonancia cerca de los 2.4 kHz:
// ahi es donde suena MAS FUERTE con la misma potencia.
#define BUZZ_FREQ_HZ   2400
#define BUZZ_MS         300      // duracion del bip al recibir

// ============================================================
//  IDENTIFICACION DE NODO
// ============================================================
// Cada placa ignora sus propios paquetes usando este ID.
// Lo define platformio.ini con -D NODE_ID=<n> segun el entorno:
//    pio run -e nodo1 -t upload   -> placa A (ID 1)
//    pio run -e nodo2 -t upload   -> placa B (ID 2)
// El valor de abajo es solo el respaldo si se compila sin esa bandera.
#ifndef NODE_ID
#define NODE_ID        1
#endif

// Byte magico: filtra basura de otros equipos que anden en 433 MHz.
#define MAGIC_BYTE     0xA7

// Repeticiones del paquete por cada pulsacion.
#define TX_REPEAT       3
#define TX_REPEAT_MS   30

// Modo baliza: manda un paquete cada X ms automaticamente.
#define BEACON_PERIOD_MS 1000

// ============================================================
//  PULSADOR — tiempos para distinguir pulsacion corta vs larga
// ============================================================
// Corta  (< HOLD_MS)  -> manda un pulso
// Larga  (>= HOLD_MS) -> activa/desactiva el modo baliza
#define DEBOUNCE_MS      50
#define HOLD_MS        1500      // mantener apretado este tiempo = toggle baliza


// ============================================================
//  PROTOCOLO — tipos de paquete
//  (antes solo existía el pulso; ahora conviven pulso y audio)
// ============================================================
#define PKT_KIND_PULSE         0
#define PKT_KIND_AUDIO_HEADER  1
#define PKT_KIND_AUDIO_DATA    2
#define PKT_KIND_AUDIO_END     3
#define PKT_KIND_ALERTA        4

// ============================================================
//  AUDIO sobre RF — formato del paquete
// ============================================================
// FIFO del CC1101 = 64 bytes: 1 de largo + 61 de payload + 2 de CRC.
//
// Cabecera de 8 bytes:
//   0     MAGIC
//   1     NODE_ID del que transmite
//   2     KIND
//   3-4   SEQ (para contar perdidos)
//   5-6   predictor ADPCM al empezar el bloque (int16, little endian)
//   7     indice ADPCM al empezar el bloque (0..88)
//
// El predictor y el indice viajan en CADA paquete a proposito: hacen que el
// paquete se pueda decodificar SOLO. Si se pierde uno, el error no se
// arrastra a los que siguen (verificado: la SNR despues del hueco queda
// igual que antes). Cuestan 3 bytes de los 61.
#define AUDIO_FMT_VERSION         1     // sube si cambia el formato; el RX lo chequea
#define AUDIO_DATA_HDR_LEN        8
#define ADPCM_BYTES_PER_PKT      52
#define ADPCM_SAMPLES_PER_PKT   (ADPCM_BYTES_PER_PKT * 2)              // 104
#define AUDIO_PKT_LEN           (AUDIO_DATA_HDR_LEN + ADPCM_BYTES_PER_PKT)  // 60

// 104 muestras a 8 kHz = 13 ms de audio por paquete, contra ~6.9 ms que
// cuesta mandarlo (aire + SPI + PLL + guarda). Ocupacion del canal ~53 %:
// por eso se puede transmitir en tiempo real. Con PCM de 16 bits eran
// 3.5 ms de audio por paquete y la ocupacion pasaba de 100 % (imposible).

// Guarda entre paquetes. Con la recalibracion desactivada y sin el FlushRX
// redundante, el receptor da vuelta el FIFO en ~200 us, asi que 1 ms sobra.
// Antes eran 4 ms: 3.4 s de los 9 s de transmision eran esta espera.
#define AUDIO_INTERPACKET_DELAY_MS  1
#define AUDIO_HEADER_REPEAT         2
#define AUDIO_END_REPEAT            3

// Jitter buffer: muestras (a 8 kHz) que hay que juntar antes de arrancar a
// reproducir. 3 paquetes = 39 ms, suficiente para tapar un paquete perdido
// sin que la latencia se note.
#define JITTER_PREBUFFER        (ADPCM_SAMPLES_PER_PKT * 3)

// Ring de recepcion. TIENE que ser potencia de 2 (se indexa con mascara).
#define RX_RING_MUESTRAS         2048      // ~256 ms a 8 kHz

// Tamanos de bloque de trabajo.
//   TX: muestras a 16 kHz que se leen del mic por vuelta (8 ms de audio).
//   RX: muestras a 8 kHz que se sacan del ring por vuelta; al interpolar se
//       duplican, y ese resultado no puede pasar de BLOQUE_MAX en Audio.cpp.
#define BLOQUE_TX_MUESTRAS        128
#define BLOQUE_RX_MUESTRAS         64

// Se cierra la sesion de RX si no llega nada en este tiempo. Con streaming
// esto es lo que detecta que el otro solto el pulsador.
#define AUDIO_RX_TIMEOUT_MS      250UL

// Techo de transmision, por si el pulsador queda trabado.
#define PTT_MAX_MS             10000UL
// Cuanto transmite el comando 'a' del serial (ahi no hay "mantener apretado").
#define TX_SERIAL_MS            3000UL

// Modo tono en el pulsador ('k'): el PTT manda este tono por RF en vez del
// microfono, durante este tiempo y sin importar cuando se suelte el boton.
#define PTT_TONO_HZ             1000UL
#define PTT_TONO_MS             2000UL

// ============================================================
//  PRUEBA DE ALCANCE — 'e' emisor, 'p' medidor
// ============================================================
// Para MEDIR el enlace en vez de estimarlo. Los dos roles van en teclas
// SEPARADAS a proposito: la radio es half-duplex, asi que una placa que emite
// y mide a la vez se tapa a si misma. Con los dos roles en la misma tecla,
// activarla en las dos puntas hacia que se pisaran y llegaba el 5 % de los
// paquetes (medido en la mesa).
//
//   'e' en la placa que CAMINA  -> emite y nada mas
//   'p' en la placa de la NOTEBOOK -> mide e informa, no emite
//
// Diferencias con la baliza normal ('b'), las dos necesarias para medir:
//
//  1. Manda UN paquete por numero de secuencia, sin las TX_REPEAT=3
//     repeticiones. Las repeticiones son buenas en uso real (no se pierde un
//     aviso) pero ESCONDEN la perdida: con 3 copias un enlace al 60 % parece
//     perfecto, y es justo el dato que se quiere medir.
//  2. Va mas rapido que 1/s, asi cada punto de medicion lleva segundos en vez
//     de minutos y la estadistica alcanza para que el porcentaje signifique
//     algo.
//
// El informe sale por TIEMPO, no cada N paquetes: en el borde del alcance no
// llegan N paquetes nunca, y contar paquetes dejaba la pantalla muda justo en
// el punto mas interesante. Por tiempo siempre sale una linea, y "0 paquetes"
// es un dato tan valido como cualquier otro.
//
// El medidor no bipea: el bip bloquea 300 ms y a 200 ms de periodo perderia la
// mitad de los paquetes, midiendo su propio buzzer. Queda el LED de RX.
#define ALCANCE_PERIOD_MS        200    // emisor: 5 paquetes por segundo
#define ALCANCE_INFORME_MS      5000    // medidor: una linea cada 5 s, pase lo que pase

// Cuantos paquetes tienen que entrar en una ventana si no se pierde ninguno.
// La perdida se mide contra esto y no contra los huecos de secuencia: una
// ventana en la que no llego NADA no tiene seq contra el que medirse, y asi
// le cargaba el hueco a la ventana siguiente. Contra el tiempo, una ventana
// vacia es 100 % de perdida, que es la verdad.
#define ALCANCE_ESPERADOS   (ALCANCE_INFORME_MS / ALCANCE_PERIOD_MS)

#endif // CONFIG_H