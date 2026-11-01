# GeriVoz — comunicador de voz por RF para residencias geriátricas

Firmware de un enlace de voz half-duplex (tipo *handy*) entre nodos ESP32 con transceptores CC1101 en
433,92 MHz. Cada nodo capta voz con un micrófono MEMS I2S, la filtra, la comprime y la transmite por
paquetes. Pensado como alternativa inalámbrica y de bajo costo a los sistemas cableados de llamada a
enfermería.

Proyecto integrador de **Electrónica Aplicada III** (Ingeniería Electrónica, UTN Facultad Regional
Paraná). Presentado en el **6° Congreso Virtual "Los Microcontroladores y sus Aplicaciones"**
(UTN FRP, 2026) con el trabajo *"GeriVoz: comunicador de voz por RF para residencias geriátricas"*.

> **English summary.** Half-duplex push-to-talk voice link between ESP32 nodes using CC1101
> transceivers at 433.92 MHz, intended as a low-cost wireless nurse-call system for elderly care
> homes. Audio is captured over I2S at 16 kHz, decimated to 8 kHz with a 31-tap Q15 FIR filter
> (polyphase interpolator on receive) and compressed with 4-bit IMA ADPCM (32 kbps), so it fits the
> ~90 kbps usable RF channel and can be streamed while recording. The published version (PCM, no
> compression) reached 30 m with high packet loss; this repository contains the later ADPCM
> streaming version, verified on the bench. Team project, UTN Paraná, 2026.

## Equipo

Lucas Adriel Cusit, Tomás Polinelli, Alan Monzón Cáceres, Matias Vicentin, Emiliano Rossi y
Mario Di Paola (autores del trabajo presentado en el congreso).

**Mi aporte (Matias Vicentin):** idea general y planteo de la solución, diseño 3D y desarrollo de
gran parte del firmware.

Este es el repositorio de presentación del proyecto, con el estado final del código. El desarrollo
se hizo en un repositorio de trabajo compartido por el equipo.

## Arquitectura

```
mic INMP441 --I2S 16 kHz--> FIR decimador --8 kHz--> ADPCM 4 bits --> CC1101 (2-FSK, 433,92 MHz)
                                                                          |
parlante <--I2S 16 kHz-- FIR interpolador <--8 kHz-- ADPCM <-- jitter buffer
(MAX98357A)
```

Cada nodo: ESP32 (NodeMCU-32S), CC1101 por SPI, micrófono INMP441 y amplificador MAX98357A sobre un
único bus I2S, pulsador de voz (PTT), pulsador de alerta, LEDs de TX/RX y buzzer. En el prototipo
presentado, la alimentación es un pack 2S de celdas 18650 con BMS y regulador a 3,3 V.

Funciones:

- **Voz:** mientras se mantiene el pulsador se graba, comprime y transmite; el otro nodo reproduce a
  medida que recibe.
- **Alerta:** un paquete de 5 bytes hace sonar una alarma de 2 s en el otro nodo. Al no transportar
  audio, tolera un enlace mucho peor que la voz.

## Por qué hace falta comprimir

Con la configuración usada, el CC1101 transmite a 99,98 kbps:

```
R_DATA = (256 + DRATE_M) × 2^DRATE_E × f_osc / 2^28
       = (256 + 248) × 2^11 × 26 MHz / 2^28 = 99,98 kbps
```

De cada paquete de 68 bytes en el aire, 61 son carga útil: quedan unos 89,7 kbps. El audio PCM a
8 kHz y 16 bits necesita 128 kbps, así que el canal no alcanza para transmitir en tiempo real. Esa es
la causa de fondo de la latencia y las pérdidas medidas en la primera versión (ver Resultados).

Con ADPCM IMA de 4 bits el audio pasa a 32 kbps:

| | PCM 16 bits | ADPCM 4 bits |
|---|---|---|
| Tasa de audio | 128 kbps | 32 kbps |
| Audio por paquete | 3,5 ms | 13,0 ms |
| Tiempo de aire por paquete | 10,5 ms | ~6,9 ms |
| Ocupación del canal | 300 % (no entra) | 53 % |

Cada paquete lleva en su cabecera el predictor y el índice del ADPCM, por lo que se decodifica de
forma independiente: un paquete perdido no arrastra error a los siguientes.

## Filtrado

El audio se muestrea a 16 kHz y se lleva a 8 kHz con un FIR pasabajos de 31 coeficientes (ventana
de Hamming, f<sub>c</sub> = 3500 Hz, aritmética Q15 con ganancia unitaria en continua). Reemplaza al
promedio de dos muestras de la primera versión: la peor atenuación en la banda de 4 a 8 kHz pasa de
−3,4 dB a −31,4 dB. El mismo juego de coeficientes funciona como interpolador polifase para volver a
16 kHz en recepción.

```python
# Diseño de los coeficientes (src/Dsp.cpp)
import math
N, FC, FS = 31, 3500.0, 16000
m, ft = (N - 1) / 2, FC / FS
h = [(2 * ft if abs(n - m) < 1e-12 else
      math.sin(2 * math.pi * ft * (n - m)) / (math.pi * (n - m)))
     * (0.54 - 0.46 * math.cos(2 * math.pi * n / (N - 1)))
     for n in range(N)]
h = [x / sum(h) for x in h]
q = [int(round(x * 32768)) for x in h]
q[N // 2] += 32768 - sum(q)          # suma exacta = 32768
```

## Resultados

### Versión presentada en el congreso (PCM, sin compresión)

- Pulsos de prueba: recepción a unos 100 m en línea recta sin obstáculos y a unos 40 m con obstáculos
  (puertas, paredes, escaleras).
- Voz: enlace establecido a 30 m sin obstáculos, con una pérdida de aproximadamente el 90 % de los
  paquetes y una latencia de unos 9 s entre captura y reproducción.

### Versión de este repositorio (ADPCM con streaming)

Mediciones en banco, con las dos placas:

| Ensayo | Resultado |
|---|---|
| 3 s de voz, emisor a 0 dBm | 231 paquetes enviados, 229 recibidos; transmisión a 1,00× tiempo real; reproducción sin cortes |
| Tono de 1 kHz por RF, en ambos sentidos | 230 de 230 paquetes, sin underruns |

Validación de los algoritmos, reimplementados en Python con aritmética entera idéntica:

| Prueba | Resultado |
|---|---|
| Decimador vs. filtrado + submuestreo | diferencia 0 (bit a bit) |
| Interpolador polifase vs. inserción de ceros + filtro | diferencia 0 (bit a bit) |
| Remuestreo de un tono puro: peor espuria | −57 dB |
| ADPCM sobre señal tipo voz | 25,5 dB de SNR |

La latencia de punta a punta estimada a partir de los buffers (jitter buffer de 39 ms y DMA de I2S
de 32 ms) es de unos 80 ms. **Pendiente:** medirla en las placas y repetir el ensayo de alcance a
30 m con esta versión.

### Hallazgo de RF: saturación a corta distancia

Con las dos placas sobre la misma mesa a 10 dBm, el receptor no decodificó ningún paquete, aun con
RSSI alto (−23 dBm contra −75 dBm de piso). Entre −30 y 0 dBm se recibieron 7 de 7 pulsos; a 5, 7 y
10 dBm, ninguno. Es saturación del front-end del receptor, no un error de software. Para ensayos de
mesa la potencia se baja en caliente (comando `w`); a distancia se usan los 10 dBm por defecto.

## Estructura

```
include/Config.h     Parámetros y pinout
src/main.cpp         Máquina de estados, radio y comandos de diagnóstico
src/Dsp.cpp          FIR decimador 16→8 kHz e interpolador polifase 8→16 kHz
src/Adpcm.cpp        Códec ADPCM IMA de 4 bits
src/Audio.cpp        I2S: captura y reproducción no bloqueantes
src/prueba_hw.cpp    Prueba de hardware de audio independiente (entorno prueba_audio)
tools/probar.py      Chequeo previo de las dos placas por puerto serie
tools/medir_alcance.py  Registro de la prueba de alcance
```

## Conexiones

| CC1101 | ESP32 | | Audio | ESP32 | | Interfaz | ESP32 |
|---|---|---|---|---|---|---|---|
| SCK | GPIO18 | | BCLK | GPIO26 | | PTT (activo alto) | GPIO12 |
| MISO | GPIO19 | | WS / LRC | GPIO25 | | Alerta (activo alto) | GPIO4 |
| MOSI | GPIO23 | | Datos micrófono | GPIO32 | | LED TX | GPIO27 |
| CSN | GPIO5 | | Datos parlante | GPIO22 | | LED RX | GPIO14 |
| GDO0 | GPIO2 | | | | | Buzzer (vía transistor) | GPIO33 |

GPIO12 es un pin de *strapping* del ESP32: la placa no debe alimentarse ni reiniciarse con el
pulsador PTT presionado.

## Compilación

Proyecto de PlatformIO (framework Arduino, librería `SmartRC-CC1101-Driver-Lib`). El firmware es el
mismo para las dos placas; el identificador de nodo lo define el entorno:

```bash
pio run -e nodo1 -t upload     # placa A
pio run -e nodo2 -t upload     # placa B
pio device monitor             # 115200 baudios
```

El monitor serie acepta comandos de diagnóstico de audio y RF (tono local y por RF, medidor de nivel
del micrófono, volcado de registros del CC1101, medidor de RSSI, estadísticas de enlace, entre otros);
la lista completa está en `src/main.cpp`.
