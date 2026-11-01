# GeriVoz — comunicador de voz por RF para residencias geriátricas

Sistema de llamado inalámbrico para residencias geriátricas sobre ESP32 y transceptores CC1101 en
433,92 MHz. Cada residente tiene un *handy* con el que pide **atención** o avisa una **emergencia** y
habla por **voz** con una **base** central. La base se conecta a una PC y muestra en una página web
quién llamó, con qué nivel de urgencia, y permite escuchar y descargar la voz. Pensado como
alternativa de bajo costo a los sistemas cableados de llamada a enfermería.

Proyecto integrador de **Electrónica Aplicada III** (Ingeniería Electrónica, UTN Facultad Regional
Paraná). Presentado en el **6° Congreso Virtual "Los Microcontroladores y sus Aplicaciones"**
(UTN FRP, 2026) con el trabajo *"GeriVoz: comunicador de voz por RF para residencias geriátricas"*.

> **English summary.** Low-cost wireless nurse-call system for elderly care homes: ESP32 handsets
> with CC1101 transceivers at 433.92 MHz send two urgency levels (attention / emergency) and
> half-duplex push-to-talk voice to a base station, which forwards events and decoded audio to a
> Web Serial dashboard on a PC. Approved in the course evaluation with a link beyond 100 m in line
> of sight. Audio is captured over I2S at 16 kHz, decimated to 8 kHz with a 31-tap Q15 FIR filter
> (polyphase interpolator on receive) and compressed with 4-bit IMA ADPCM (32 kbps), so it fits the
> ~90 kbps usable RF channel and can be streamed while recording. The published version (PCM, no
> compression) reached 30 m with high packet loss; this repository contains the later ADPCM
> streaming version. Team project, UTN Paraná, 2026.

## Equipo

Lucas Adriel Cusit, Tomás Polinelli, Alan Monzón Cáceres, Matias Vicentin, Emiliano Rossi y
Mario Di Paola (autores del trabajo presentado en el congreso).

**Mi aporte (Matias Vicentin):** idea general y planteo de la solución, diseño 3D y desarrollo de
gran parte del firmware.

La página web de la central es de Alan Monzón Cáceres.

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
único bus I2S, pulsador de voz (PTT), pulsador de mensajes, LEDs de TX/RX y buzzer. En el prototipo
presentado, la alimentación es un pack 2S de celdas 18650 con BMS y regulador a 3,3 V.

### Roles

El firmware es el mismo para todas las placas; el entorno de compilación define el rol y el
identificador (`NODE_ID`), que viaja en cada paquete y permite a la base saber qué residente llamó.

- **Base** (`NODE_ID` 0): acepta paquetes de todos los handies y reenvía a la PC los eventos y el audio decodificado.
- **Handy** (`NODE_ID` 1 a 18): solo acepta paquetes de la base; ignora a los otros handies.

Los paquetes de otros sistemas en la misma banda se descartan por un byte de identificación y el CRC.

### Funciones

| Acción en el handy | Qué recibe la base | En la página |
|---|---|---|
| Botón de mensajes, 1 toque | Pulso con el ID del residente; bip del buzzer | Atención general |
| Botón de mensajes, 2 toques | Alerta; sirena de 2 s por el parlante | Emergencia |
| PTT mantenido | Voz en tiempo real por el parlante | Llamada de voz, para escuchar o descargar en .wav |

La base responde por voz con su propio PTT (half-duplex: mientras uno habla, el otro escucha).

### Base → PC

La base envía por USB, a 921600 baudios, el texto de cada evento y el audio ya decodificado en
tramas binarias (sincronismo, nodo, número de secuencia, muestras y XOR de control). La página
(`pagina/`, Web Serial en Chrome o Edge) separa texto y audio, rellena con silencio los paquetes
perdidos y arma un WAV de 8 kHz por llamada.

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

### Evaluación de la cátedra (14/10/2026)

Sistema completo (base, handy y página) **aprobado**, con enlace a más de 100 m sin obstáculos. Se
evaluó la identificación del residente, los dos niveles de urgencia y la llamada de voz.

### Versión presentada en el congreso (PCM, sin compresión)

- Pulsos de prueba: recepción a unos 100 m en línea recta sin obstáculos y a unos 40 m con obstáculos
  (puertas, paredes, escaleras).
- Voz: enlace establecido a 30 m sin obstáculos, con una pérdida de aproximadamente el 90 % de los
  paquetes y una latencia de unos 9 s entre captura y reproducción.

### Versión con ADPCM y streaming: mediciones en banco

Con las dos placas sobre la mesa:

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
de 32 ms) es de unos 80 ms; no se midió de punta a punta.

### Hallazgo de RF: saturación a corta distancia

Con las dos placas sobre la misma mesa a 10 dBm, el receptor no decodificó ningún paquete, aun con
RSSI alto (−23 dBm contra −75 dBm de piso). Entre −30 y 0 dBm se recibieron 7 de 7 pulsos; a 5, 7 y
10 dBm, ninguno. Es saturación del front-end del receptor, no un error de software. Para ensayos de
mesa la potencia se baja en caliente (comando `w`); a distancia se usan los 10 dBm por defecto.

## Estructura

```
include/Config.h     Parámetros y pinout
src/main.cpp         Máquina de estados, roles, radio, envío a la PC y comandos de diagnóstico
src/Dsp.cpp          FIR decimador 16→8 kHz e interpolador polifase 8→16 kHz
src/Adpcm.cpp        Códec ADPCM IMA de 4 bits
src/Audio.cpp        I2S: captura y reproducción no bloqueantes
src/prueba_hw.cpp    Prueba de hardware de audio independiente (entorno prueba_audio)
tools/probar.py      Chequeo previo de las dos placas por puerto serie
tools/medir_alcance.py  Registro de la prueba de alcance
pagina/              Panel de la central (HTML + JavaScript, Web Serial)
```

## Conexiones

| CC1101 | ESP32 | | Audio | ESP32 | | Interfaz | ESP32 |
|---|---|---|---|---|---|---|---|
| SCK | GPIO18 | | BCLK | GPIO26 | | PTT (activo alto) | GPIO12 |
| MISO | GPIO19 | | WS / LRC | GPIO25 | | Mensajes (activo alto) | GPIO4 |
| MOSI | GPIO23 | | Datos micrófono | GPIO32 | | LED TX | GPIO27 |
| CSN | GPIO5 | | Datos parlante | GPIO22 | | LED RX | GPIO14 |
| GDO0 | GPIO2 | | | | | Buzzer (vía transistor) | GPIO33 |

GPIO12 es un pin de *strapping* del ESP32: la placa no debe alimentarse ni reiniciarse con el
pulsador PTT presionado.

## Compilación

Proyecto de PlatformIO (framework Arduino, librería `SmartRC-CC1101-Driver-Lib`). El rol y el
identificador los define el entorno:

```bash
pio run -e base   -t upload    # central, va conectada a la PC
pio run -e handy1 -t upload    # handy del residente 1 (handy2, ... para más residentes)
pio device monitor             # 921600 baudios
```

Para usar la página, cerrar el monitor serie, abrir `pagina/index.html` en Chrome o Edge y conectar
la base a 921600 baudios. Los nombres de cada residente se configuran al principio de `pagina/app.js`.

El monitor serie acepta comandos de diagnóstico de audio y RF (tono local y por RF, medidor de nivel
del micrófono, volcado de registros del CC1101, medidor de RSSI, estadísticas de enlace, entre otros);
la lista completa está en `src/main.cpp`.
