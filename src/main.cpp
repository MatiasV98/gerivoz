#include <Arduino.h>
#include <string.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#include "Config.h"
#include "Audio.h"
#include "Dsp.h"
#include "Adpcm.h"
#include "esp_task_wdt.h"

// ============================================================
//  ENLACE DE VOZ CC1101 — streaming
// ============================================================
// Antes: se grababan 3 s completos, se decimaba, se mandaba TODO, y del otro
// lado se reproducia TODO al final. Eran ~15 s para 3 s de voz, y el cuello
// no era el software: audio sin comprimir a 8 kHz y 16 bits son 128 kbps,
// contra ~89.7 kbps utiles que da el CC1101 a 99.98 kbps brutos. No entraba
// en el canal ni con delay cero.
//
// Ahora: ADPCM 4:1 (32 kbps, el mismo bitrate que G.726 en los telefonos
// DECT) baja la ocupacion del canal al ~53 %, y recien ahi se puede mandar
// mientras se graba y reproducir mientras llega. La latencia de punta a
// punta pasa de ~15 s a ~80 ms.

// La tarea de Arduino (loopTask) NO viene suscrita al task watchdog, asi que
// esp_task_wdt_reset() a secas devuelve "task not found" y loguea un error por
// cada llamada. Medido: 462 lineas en una sola corrida de 'm', que a 115200
// son ~25 KB = ~2,2 s de serie bloqueando. En el camino del audio eso rompe el
// timing y provoca underruns.
// Suscribiendo la tarea en setup() el reset vale de verdad y no loguea nada.
static bool wdtSuscrito = false;

static inline void feedTheDog()
{
    if (wdtSuscrito) esp_task_wdt_reset();
}

// ---------- Estado del pulsador ----------
static bool          btnLast     = LOW;
static bool          btnState    = LOW;
static unsigned long btnLastTime = 0;

// ---------- Contadores de PULSO ----------
static uint16_t      txSeq        = 0;
static uint32_t      rxTotal      = 0;
static uint16_t      rxUltimaSeq  = 0;
static bool          rxHuboAlguno = false;
static uint32_t      rxPerdidos   = 0;

// ---------- Modo baliza ----------
static bool          beaconOn    = false;
static unsigned long beaconLast  = 0;

// ---------- Prueba de alcance ('e' emisor / 'p' medidor) ----------
// Roles separados: la radio es half-duplex y una placa que emite y mide a la
// vez se tapa a si misma. Los acumuladores son aparte de rxTotal/rxPerdidos
// para no pisar las estadisticas de siempre que informa 's'.
static bool          alcEmisor      = false;
static bool          alcMedidor     = false;
static unsigned long alcEmisorLast  = 0;
static unsigned long alcInformeLast = 0;
static uint32_t      alcRecibidos   = 0;
static int32_t       alcRssiSuma    = 0;
static int           alcRssiMin     = 0, alcRssiMax = 0;
static uint32_t      alcLqiSuma     = 0;
static uint8_t       alcLqiPeor     = 0;
static uint32_t      alcVentana     = 0;   // numero de informe, para la planilla

// ---------- AUDIO ----------
Audio audio;

static FirDecimador     dec;        // 16 kHz -> 8 kHz (camino de transmision)
static FirInterpolador  interp;     // 8 kHz -> 16 kHz (camino de recepcion)

// Ring de recepcion: hace de jitter buffer. Guarda muestras a CODEC_RATE.
static int16_t  rxRing[RX_RING_MUESTRAS];
static uint16_t rxRd = 0, rxWr = 0, rxN = 0;

// Staging de salida: muestras ya interpoladas a 16 kHz esperando lugar en el
// DMA. Hace falta porque escribirParlante() puede aceptar solo una parte, y
// el interpolador tiene estado: no se puede "volver a generar" lo que no
// entro sin duplicar muestras.
static int16_t outStage[BLOQUE_RX_MUESTRAS * 2 + 8];
static int     outStageN = 0, outStageRd = 0;

static bool          audioReceiving = false;   // hay sesion abierta
static bool          audioPlaying   = false;   // ya arranco el parlante
static bool          audioCerrando  = false;   // llego END o timeout: drenar
static bool          enUnderrun     = false;   // para no contar el mismo mil veces

// Para saber si la reproduccion salio CONTINUA. El contador de underruns solo
// dice "el ring se vacio", que es inofensivo mientras el DMA todavia tenga
// audio. Lo que importa es si el sonido tardo mas de lo que dura: si se
// reprodujeron N muestras de audio en mas tiempo del que esas N muestras
// duran, hubo cortes de verdad.
static unsigned long playT0 = 0;
static uint32_t      muestrasSalida = 0;
static uint16_t      picoSalida     = 0;
// Primeras muestras decodificadas de la sesion, para poder VER lo que llega
// en vez de confiar solo en los contadores.
#define CAPTURA_N 48
static int16_t       captura[CAPTURA_N];
static uint8_t       capturadas = 0;
static uint64_t      sumaSalida     = 0;
static unsigned long audioUltimoPaquete = 0;
static uint16_t      audioSeqEsperada   = 0;
static uint32_t      audioRecibidos = 0, audioPerdidos = 0, audioUnderruns = 0;
static uint32_t      rfTimeouts     = 0;
static uint32_t      pttPulsaciones = 0;   // veces que se uso el pulsador

// ---------- Segundo pulsador: ALERTA ----------
static bool          altLast     = LOW;
static bool          altState    = LOW;
static unsigned long altLastTime = 0;
static uint32_t      alertasEnviadas  = 0;
static uint32_t      alertasRecibidas = 0;

// Con esto en true el pulsador manda un tono fijo en vez de lo que capta el
// microfono. Para probar el enlace de punta a punta con una senial conocida
// cuando el microfono no es de fiar. Se conmuta en caliente con 'k'.
static bool          pttTono   = false;
static uint32_t      pttTonoHz = PTT_TONO_HZ;

// Contadores de diagnostico del camino de recepcion. Sirven para saber DONDE
// se pierden los paquetes cuando llega energia pero no entra nada.
static uint32_t rxFlags     = 0;   // GDO0 se activo: el chip engancho un sync
static uint32_t rxLenMala   = 0;   // largo invalido
static uint32_t rxCrcMal    = 0;   // CRC malo
static uint32_t rxMagicMal  = 0;   // no es nuestro MAGIC_BYTE
static uint32_t rxPropio    = 0;   // es un paquete nuestro (mismo NODE_ID)
static uint32_t rxOverflow  = 0;   // el FIFO se desbordo (no se leyo a tiempo)
static bool     rxVerboso   = false;

// ============================================================
//  BUZZER
// ============================================================
#define BUZZ_LEDC_CH   0

static void buzzerInit()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_BUZZER, BUZZ_FREQ_HZ, 8);
    ledcWrite(PIN_BUZZER, 0);
#else
    ledcSetup(BUZZ_LEDC_CH, BUZZ_FREQ_HZ, 8);
    ledcAttachPin(PIN_BUZZER, BUZZ_LEDC_CH);
    ledcWrite(BUZZ_LEDC_CH, 0);
#endif
}

static void buzzerOn(uint32_t freq)
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(PIN_BUZZER, freq);
#else
    ledcWriteTone(BUZZ_LEDC_CH, freq);
    ledcWrite(BUZZ_LEDC_CH, 128);
#endif
}

static void buzzerOff()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_BUZZER, 0);
#else
    ledcWrite(BUZZ_LEDC_CH, 0);
#endif
}

static void bip(uint32_t freq, uint32_t ms)
{
    buzzerOn(freq);
    delay(ms);
    buzzerOff();
}

// ============================================================
//  RADIO
// ============================================================

// Flush "de verdad": SIDLE -> SFRX -> SRX. Solo para el camino de error,
// porque pasar por IDLE es lo que dispara la recalibracion del PLL.
void FlushRX()
{
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
}

// Calibracion manual del sintetizador. Con FS_AUTOCAL apagado (ver
// initCC1101) el chip no recalibra solo, asi que hay que hacerlo a mano:
// una vez al arrancar y una vez antes de cada tanda de transmision. Son
// ~721 us, contra los mismos ~721 us que antes se pagaban EN CADA PAQUETE.
// Espera a que la maquina de estados llegue a IDLE (MARCSTATE = 1).
static bool esperarIdle(uint32_t limiteUs)
{
    uint32_t t0 = micros();
    while ((ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE) & 0x1F) != 0x01) {
        if (micros() - t0 > limiteUs) return false;
    }
    return true;
}

static bool calibrarRF()
{
    // SCAL SOLO vale desde IDLE: en cualquier otro estado el chip lo ignora.
    // setSidle() estroba SIDLE pero no espera a que la transicion ocurra, asi
    // que hay que esperarla a mano. Si no, la calibracion no corre nunca y,
    // con FS_AUTOCAL apagado (MCSM0 = 0x08), el sintetizador se queda con los
    // valores estaticos de FSCAL que escribio la libreria: anda o no anda
    // segun el chip y la temperatura.
    ELECHOUSE_cc1101.setSidle();
    if (!esperarIdle(10000)) {
        Serial.println("[RF] No llego a IDLE para calibrar");
        ELECHOUSE_cc1101.SetRx();
        return false;
    }

    ELECHOUSE_cc1101.SpiStrobe(CC1101_SCAL);
    delayMicroseconds(50);              // dejar que entre en CALIBRATE
    bool ok = esperarIdle(10000);       // la calibracion tarda ~721 us
    if (!ok) Serial.println("[RF] La calibracion no termino");

    ELECHOUSE_cc1101.SetRx();
    return ok;
}

// Transmision rapida. La SendData() de la libreria hace SIDLE + STX, y ese
// paso por IDLE cuesta una recalibracion de 721 us por paquete. Estrobando
// STX directo desde RX no se pasa por IDLE, y con TXOFF_MODE=RX el chip
// vuelve solo a recibir cuando termina.
static void rfEnviar(const uint8_t* buf, uint8_t len)
{
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, len);
    ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_TXFIFO, (uint8_t*)buf, len);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);

    // GDO0 (IOCFG0 = 0x06) sube cuando salio el sync word y baja al final
    // del paquete. Esperar los dos flancos = esperar exactamente lo que dura
    // la transmision, sin delays fijos de mas.
    uint32_t t0 = micros();
    while (!digitalRead(PIN_GDO0)) {
        if (micros() - t0 > 20000) {
            rfTimeouts++;
            ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
            FlushRX();
            return;
        }
    }

    t0 = micros();
    while (digitalRead(PIN_GDO0)) {
        if (micros() - t0 > 20000) {
            rfTimeouts++;
            ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
            FlushRX();
            return;
        }
    }
}

// Recepcion propia, en vez de ReceiveData() + CheckCRC() de la libreria.
//
// El problema de la libreria: CheckCRC() mira el bit CRC_OK del registro LQI,
// que segun el datasheet "se borra al entrar o reiniciar RX". ReceiveData()
// termina estrobando SFRX+SRX, y ademas con RXOFF_MODE=RX el chip reanuda la
// busqueda apenas termina el paquete. En los dos casos el bit ya no esta
// cuando se lo consulta, asi que TODO paquete daba CRC malo y se descartaba,
// aunque llegara perfecto (verificado en la placa).
//
// Con APPEND_STATUS (PKTCTRL1 = 0x04) el chip agrega dos bytes al final del
// paquete EN EL PROPIO FIFO: RSSI y LQI+CRC_OK. Esos viajan con el paquete,
// asi que no dependen del estado del chip. Es la fuente confiable.
static bool rfRecibir(uint8_t* buf, uint8_t bufMax, uint8_t* lenOut,
                      bool* crcOk, int* rssiOut, uint8_t* lqiOut)
{
    *lenOut = 0;
    *crcOk  = false;

    uint8_t enFifo = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES) & 0x7F;
    if (enFifo < 3) {                       // ni largo + 2 de estado
        FlushRX();
        return false;
    }

    uint8_t size = ELECHOUSE_cc1101.SpiReadReg(CC1101_RXFIFO);
    if (size == 0 || size > bufMax || (uint16_t)size + 2 > enFifo) {
        FlushRX();
        return false;
    }

    ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, buf, size);

    uint8_t estado[2] = {0, 0};
    ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, estado, 2);

    int rssiRaw = estado[0];
    *rssiOut = (rssiRaw >= 128) ? ((rssiRaw - 256) / 2 - 74) : (rssiRaw / 2 - 74);
    *lqiOut  = estado[1] & 0x7F;
    *crcOk   = (estado[1] & 0x80) != 0;
    *lenOut  = size;

    return true;
}

static bool initCC1101()
{
    ELECHOUSE_cc1101.setSpiPin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
    ELECHOUSE_cc1101.setGDO0(PIN_GDO0);
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setCCMode(1);
    ELECHOUSE_cc1101.setMHZ(RF_FREQ_MHZ);

    // 2-FSK. SIN ESTO NO HAY ENLACE: nadie llamaba a setModulation(), asi que
    // MDMCFG2 quedaba en el valor de reset del chip, que es ASK/OOK. Verificado
    // leyendo el registro en la placa: MDMCFG2 = 0x32 (MOD_FORMAT = 011 = OOK).
    // OOK ademas necesita otra tabla de PA (FREND0 con dos entradas) y otra AGC,
    // que tampoco estaban puestas, asi que no transmitia nada util.
    // Con 2-FSK cierra todo: desviacion 47.6 kHz (DEVIATN = 0x47) y RxBW
    // 203 kHz (MDMCFG4 = 0x8B) son justo lo que pide Carson para 100 kbps.
    ELECHOUSE_cc1101.setModulation(0);

    ELECHOUSE_cc1101.setPA(RF_PA_DBM);

    // --- Ajustes para bajar la latencia ---------------------------------
    // No se toca el data rate: setCCMode(1) deja 99.98 kbps, y subirlo a
    // 250 costaria sensibilidad (o sea alcance, que es el otro requisito).
    // Con ADPCM ya sobra canal, asi que no hace falta.

    // 4 bytes de preambulo en vez de 2. Ahora que hay margen de tiempo,
    // conviene darle mas a enganchar al receptor. Cuesta 0.16 ms de aire.
    ELECHOUSE_cc1101.setPRE(2);

    // MCSM1 = 0x0F: CCA_MODE=00 (transmitir siempre, sin escuchar antes),
    // RXOFF_MODE=11 (quedarse en RX al terminar de recibir),
    // TXOFF_MODE=11 (pasar a RX al terminar de transmitir).
    // Con esto el chip nunca pasa por IDLE, y sin IDLE no hay recalibracion.
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MCSM1, 0x0F);

    // MCSM0 = 0x08: FS_AUTOCAL=00 (nunca). Calibramos a mano en calibrarRF().
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MCSM0, 0x08);

    bool ok = ELECHOUSE_cc1101.getCC1101();
    Serial.print("[RF] CC1101: ");
    Serial.println(ok ? "OK" : "ERROR");
    Serial.print("[RF] Frecuencia: ");
    Serial.print(ELECHOUSE_cc1101.getMHZ());
    Serial.println(" MHz");
    Serial.print("[RF] Data rate: ");
    Serial.print(ELECHOUSE_cc1101.getDRate());
    Serial.println(" kbps");
    Serial.print("[RF] Potencia: ");
    Serial.print(RF_PA_DBM);
    Serial.println(" dBm");

    bool cal = calibrarRF();
    Serial.print("[RF] Calibracion del sintetizador: ");
    Serial.println(cal ? "OK" : "FALLO");
    return ok;
}

// ============================================================
//  PULSO
// ============================================================
// 'repetir' manda TX_REPEAT copias del mismo seq: bien para que no se pierda
// un aviso, mal para medir, porque esconde la perdida. La prueba de alcance lo
// llama con false para que cada seq viaje una sola vez.
static void transmitirPulso(bool repetir = true, bool imprimir = true)
{
    uint8_t buf[5];

    txSeq++;

    buf[0] = MAGIC_BYTE;
    buf[1] = NODE_ID;
    buf[2] = PKT_KIND_PULSE;
    buf[3] =  txSeq       & 0xFF;
    buf[4] = (txSeq >> 8) & 0xFF;

    digitalWrite(LED_TX, HIGH);

    const int copias = repetir ? TX_REPEAT : 1;
    for (int i = 0; i < copias; i++) {
        rfEnviar(buf, sizeof(buf));
        if (repetir) delay(TX_REPEAT_MS);
    }

    digitalWrite(LED_TX, LOW);

    if (imprimir) {
        Serial.print(">> TX seq=");
        Serial.print(txSeq);
        Serial.print(" (x");
        Serial.print(copias);
        Serial.println(")");
    }
}

// ============================================================
//  PRUEBA DE ALCANCE — acumulacion e informe
// ============================================================

static void alcanceResetVentana()
{
    alcRecibidos = 0;
    alcRssiSuma  = 0;
    alcLqiSuma   = 0;
    alcLqiPeor   = 0;
}

// Una linea por ventana, en formato de planilla, para caminar y anotar sin
// desarmar la salida. Sale SIEMPRE, aunque no haya llegado nada: cero
// paquetes es el dato que marca donde se corta el enlace.
//
// Los esperados se calculan POR TIEMPO, no por huecos de secuencia. La
// cadencia del emisor es conocida y fija (un paquete cada ALCANCE_PERIOD_MS),
// asi que en una ventana tienen que entrar exactamente ALCANCE_ESPERADOS. Con
// huecos de secuencia, una ventana sin ningun paquete no tenia contra que
// medirse y le cargaba el hueco a la ventana siguiente, que saltaba al 80 %
// mientras la vacia informaba 0 %. Por tiempo, una ventana vacia es
// directamente 100 % de perdida, que es la verdad.
static void alcanceInforme()
{
    const uint32_t esperados = ALCANCE_ESPERADOS;
    uint32_t rx = alcRecibidos;
    if (rx > esperados) rx = esperados;         // por si entro alguno de mas
    float perdida = 100.0f * (float)(esperados - rx) / (float)esperados;

    alcVentana++;

    Serial.print("[ALCANCE] #");
    Serial.print(alcVentana);
    Serial.print(" | rx=");
    Serial.print(alcRecibidos);
    Serial.print("/");
    Serial.print(esperados);
    Serial.print(" perdida=");
    Serial.print(perdida, 1);
    Serial.print("%");

    if (alcRecibidos == 0) {
        Serial.println("  <-- SIN PAQUETES, aca no llega nada");
        alcanceResetVentana();
        return;
    }

    Serial.print(" | RSSI prom=");
    Serial.print((int)(alcRssiSuma / (int32_t)alcRecibidos));
    Serial.print(" min=");
    Serial.print(alcRssiMin);
    Serial.print(" max=");
    Serial.print(alcRssiMax);
    Serial.print(" dBm | LQI prom=");
    Serial.print((int)(alcLqiSuma / alcRecibidos));
    Serial.print(" peor=");
    Serial.println(alcLqiPeor);

    alcanceResetVentana();
}

// Vuelca los registros que importan del CC1101. Sirve para ver si la config
// que escribimos quedo de verdad en el chip y en que estado esta la maquina
// de estados (MARCSTATE), que es lo primero que hay que mirar cuando "no
// recibe": si no dice 13/RX, no esta escuchando.
static void volcarRegistrosRF()
{
    struct { const char* nombre; uint8_t dir; bool estado; } regs[] = {
        { "PARTNUM  ", 0x30, true  },
        { "VERSION  ", 0x31, true  },
        { "MARCSTATE", 0x35, true  },
        { "PKTSTATUS", 0x38, true  },
        { "IOCFG0   ", 0x02, false },
        { "PKTCTRL1 ", 0x07, false },
        { "PKTCTRL0 ", 0x08, false },
        { "FREQ2    ", 0x0D, false },
        { "FREQ1    ", 0x0E, false },
        { "FREQ0    ", 0x0F, false },
        { "MDMCFG4  ", 0x10, false },
        { "MDMCFG3  ", 0x11, false },
        { "MDMCFG2  ", 0x12, false },
        { "MDMCFG1  ", 0x13, false },
        { "MCSM1    ", 0x17, false },
        { "MCSM0    ", 0x18, false },
        { "FSCAL3   ", 0x23, false },
        { "FSCAL2   ", 0x24, false },
        { "FSCAL1   ", 0x25, false },
        { "FSCAL0   ", 0x26, false },
    };

    Serial.println("--- Registros CC1101 ---");
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t v = regs[i].estado
                  ? ELECHOUSE_cc1101.SpiReadStatus(regs[i].dir)
                  : ELECHOUSE_cc1101.SpiReadReg(regs[i].dir);
        Serial.print("  ");
        Serial.print(regs[i].nombre);
        Serial.print(" = 0x");
        if (v < 16) Serial.print("0");
        Serial.print(v, HEX);
        if (regs[i].dir == 0x12) {
            const char* mods[8] = { "2-FSK", "GFSK", "?", "ASK/OOK",
                                    "4-FSK", "?", "?", "MSK" };
            Serial.print("  (mod=");
            Serial.print(mods[(v >> 4) & 0x07]);
            Serial.print(", sync=");
            Serial.print(v & 0x07);
            Serial.print(")");
        }
        if (regs[i].dir == 0x35) {
            // 1=IDLE, 13=RX, 19=TX (tabla de MARCSTATE del datasheet)
            Serial.print("  (");
            uint8_t m = v & 0x1F;
            Serial.print(m == 1 ? "IDLE" : m == 13 ? "RX" : m == 19 ? "TX" :
                         m == 17 ? "RXFIFO_OVERFLOW" : m == 22 ? "TXFIFO_UNDERFLOW" : "otro");
            Serial.print(")");
        }
        Serial.println();
    }
    Serial.print("  GDO0 (pin ");
    Serial.print(PIN_GDO0);
    Serial.print(") = ");
    Serial.println(digitalRead(PIN_GDO0));
    Serial.print("  PULSADOR (pin ");
    Serial.print(BTN_PTT);
    Serial.print(") = ");
    Serial.print(digitalRead(BTN_PTT));
    Serial.println(digitalRead(BTN_PTT) ? "  (APRETADO)" : "  (suelto)");
    Serial.println("------------------------");
}

// Mide el nivel de RF que entra por la antena, sin importar si los paquetes
// decodifican o no. Es el corte diagnostico clave cuando "no recibe":
//   - el RSSI NO se mueve mientras el otro transmite -> no llega energia:
//     antena, PA o la red de adaptacion de la PCB.
//   - el RSSI SUBE pero no entran paquetes -> llega energia y el problema
//     esta en el formato de trama (sync, modulacion, data rate, CRC).
static void monitorRssi(unsigned long ms)
{
    Serial.print("[RF] Midiendo RSSI ");
    Serial.print(ms / 1000.0, 1);
    Serial.println(" s...");

    int  minv = 127, maxv = -127;
    long suma = 0;
    int  n = 0;

    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        int r = ELECHOUSE_cc1101.getRssi();
        if (r < minv) minv = r;
        if (r > maxv) maxv = r;
        suma += r;
        n++;
        delay(5);
        feedTheDog();
    }

    Serial.print("[RF] RSSI min=");
    Serial.print(minv);
    Serial.print(" max=");
    Serial.print(maxv);
    Serial.print(" prom=");
    Serial.print(n ? (float)suma / n : 0.0f, 1);
    Serial.print(" dBm | muestras=");
    Serial.println(n);
    FlushRX();
}

// Monitorea las seniales crudas del receptor mientras el otro transmite:
//   CS   (carrier sense)  = el chip ve portadora arriba del umbral
//   GDO0 (IOCFG0 = 0x06)  = el chip engancho un SYNC WORD valido
// Separa "no detecta nada" de "detecta portadora pero no sincroniza" de
// "sincroniza pero el paquete no llega al FIFO".
static void monitorGdo(unsigned long ms)
{
    Serial.print("[RF] Mirando CS/GDO0/FIFO ");
    Serial.print(ms / 1000.0, 1);
    Serial.println(" s...");

    uint32_t n = 0, conCS = 0, conGdo = 0, conFifo = 0;
    uint8_t  maxFifo = 0;

    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        uint8_t pk = ELECHOUSE_cc1101.SpiReadStatus(CC1101_PKTSTATUS);
        uint8_t rb = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);
        if (pk & 0x40) conCS++;                 // bit 6 = CS
        if (digitalRead(PIN_GDO0)) conGdo++;
        if ((rb & 0x7F) > 0) {
            conFifo++;
            if ((rb & 0x7F) > maxFifo) maxFifo = rb & 0x7F;
        }
        n++;
        feedTheDog();
    }

    Serial.print("[RF] muestras=");    Serial.print(n);
    Serial.print(" conCS=");           Serial.print(conCS);
    Serial.print(" conSYNC(GDO0)=");   Serial.print(conGdo);
    Serial.print(" conBytes=");        Serial.print(conFifo);
    Serial.print(" maxFIFO=");         Serial.println(maxFifo);
    FlushRX();
}

// Tono de prueba directo al parlante: ni microfono ni radio de por medio.
// Aisla la cadena I2S -> MAX98357A -> parlante.
//
// Lo que informa al final es lo que importa:
//   - si TARDA lo que tiene que durar, el I2S esta clockeando bien y el audio
//     esta saliendo del ESP32: si igual no se escucha, el problema es el
//     amplificador (pin SD) o el parlante.
//   - si termina al instante, el I2S se traga todo sin reproducir: ahi el
//     problema es la configuracion del driver, no el hardware de audio.
static void tonoPrueba(uint32_t hz, uint32_t ms, int16_t amplitud)
{
    Serial.println();
    Serial.print("[TEST] Tono ");
    Serial.print(hz);
    Serial.print(" Hz durante ");
    Serial.print(ms / 1000.0, 1);
    Serial.print(" s, amplitud ");
    Serial.print(amplitud);
    Serial.print(" (");
    Serial.print(20.0 * log10((double)amplitud / 32768.0), 1);
    Serial.println(" dBFS), en los dos canales");

    audio.modoReproducir();

    const int N = BLOQUE_RX_MUESTRAS * 2;
    int16_t   buf[BLOQUE_RX_MUESTRAS * 2];
    uint32_t  fase = 0, generadas = 0, escritas = 0, vecesLleno = 0;

    unsigned long t0 = millis();
    unsigned long limite = (unsigned long)ms + 2000;   // red de seguridad

    while (millis() - t0 < ms) {
        for (int i = 0; i < N; i++) {
            buf[i] = (int16_t)(amplitud *
                     sin(2.0 * PI * (double)fase * hz / SAMPLE_RATE));
            fase++;
        }
        generadas += N;

        int esc = 0;
        while (esc < N && (millis() - t0) < limite) {
            int w = audio.escribirParlante(&buf[esc], N - esc);
            if (w == 0) { vecesLleno++; delay(1); }
            esc += w;
            feedTheDog();
        }
        escritas += esc;
    }

    unsigned long dt = millis() - t0;
    audio.silencio();

    Serial.print("[TEST] generadas=");
    Serial.print(generadas);
    Serial.print(" escritas=");
    Serial.print(escritas);
    Serial.print(" DMA-lleno=");
    Serial.println(vecesLleno);
    Serial.print("[TEST] duro ");
    Serial.print(dt / 1000.0, 2);
    Serial.print(" s para ");
    Serial.print((float)escritas / SAMPLE_RATE, 2);
    Serial.print(" s de audio (");
    Serial.print(escritas > 0 ? (float)dt / (1000.0f * escritas / SAMPLE_RATE) : 0.0f, 2);
    Serial.println("x; 1.00 = el I2S esta clockeando bien)");
    Serial.println("[TEST] Si marca ~1.00x y NO se escucha: mirar el pin SD del");
    Serial.println("       MAX98357A (al aire = apagado; va a 3V3) y el parlante.");
}

// Barre varias frecuencias de muestreo tocando el mismo tono en cada una.
// Lo que cambia es el BCLK:  fs x 32 bits x 2 canales.
//   16 kHz -> 1.02 MHz | 32 kHz -> 2.05 MHz | 48 kHz -> 3.07 MHz
// El MAX98357A necesita un BCLK dentro de su rango para engancharse. Si suena
// en una y no en las otras, el problema era la frecuencia, no el parlante.
static void barridoBclk()
{
    static const uint32_t FS[] = { 16000, 32000, 48000 };

    Serial.println();
    Serial.println("=== Barrido de BCLK: 3 pitidos de 1 kHz, 2 s cada uno ===");
    Serial.println("    ESCUCHA CUAL SUENA Y AVISA.");

    for (unsigned k = 0; k < sizeof(FS) / sizeof(FS[0]); k++) {
        uint32_t fs = FS[k];
        Serial.print("  -> fs = ");
        Serial.print(fs);
        Serial.print(" Hz, BCLK = ");
        Serial.print(fs * 64 / 1000.0, 0);
        Serial.println(" kHz");

        audio.cambiarFrecuencia(fs);

        int16_t  buf[BLOQUE_RX_MUESTRAS * 2];
        uint32_t fase = 0;
        unsigned long t0 = millis();
        while (millis() - t0 < 2000) {
            for (int i = 0; i < BLOQUE_RX_MUESTRAS * 2; i++) {
                buf[i] = (int16_t)(12000.0 * sin(2.0 * PI * (double)fase * 1000.0 / fs));
                fase++;
            }
            int esc = 0;
            while (esc < BLOQUE_RX_MUESTRAS * 2 && (millis() - t0) < 2500) {
                int w = audio.escribirParlante(&buf[esc], BLOQUE_RX_MUESTRAS * 2 - esc);
                if (w == 0) delay(1);
                esc += w;
                feedTheDog();
            }
        }
        audio.silencio();
        delay(600);                       // silencio entre pitidos
        feedTheDog();
    }

    audio.cambiarFrecuencia(SAMPLE_RATE); // dejar el puerto como estaba
    Serial.println("=== Fin del barrido (puerto de vuelta en 16 kHz) ===");
}

// Medidor de nivel del microfono EN VIVO. Imprime una barra varias veces por
// segundo para ver si el mic reacciona a la voz: si al hablar la barra sube
// claramente, el microfono capta; si queda igual, lo que se mide es ruido.
static void medidorMic(unsigned long ms)
{
    Serial.println();
    Serial.println("=== Medidor de microfono: HABLA FUERTE Y MIRA LA BARRA ===");

    audio.modoGrabar();
    dec.reset();

    int16_t in16[BLOQUE_TX_MUESTRAS];
    int16_t out8[BLOQUE_TX_MUESTRAS];
    int32_t pico = 0;
    uint32_t cuenta = 0;

    unsigned long t0 = millis(), ultimo = millis();
    while (millis() - t0 < ms) {
        int n = audio.leerMic(in16, BLOQUE_TX_MUESTRAS, 1);
        if (n > 0) {
            int m = dec.procesar(in16, n, out8);
            for (int i = 0; i < m; i++) {
                int32_t a = out8[i] < 0 ? -(int32_t)out8[i] : out8[i];
                if (a > pico) pico = a;
                cuenta++;
            }
        }

        if (millis() - ultimo >= 200) {
            float db = pico > 0 ? 20.0f * log10((float)pico / 32768.0f) : -99.0f;
            Serial.print("  ");
            int barras = pico > 0 ? (int)((db + 60.0f) / 3.0f) : 0;
            if (barras < 0) barras = 0;
            if (barras > 20) barras = 20;
            for (int i = 0; i < barras; i++) Serial.print("#");
            for (int i = barras; i < 20; i++) Serial.print(".");
            Serial.print("  pico=");
            Serial.print(pico);
            Serial.print(" (");
            Serial.print(db, 1);
            Serial.println(" dBFS)");
            pico = 0;
            ultimo = millis();
        }
        feedTheDog();
    }

    Serial.print("=== Fin. Muestras leidas: ");
    Serial.println(cuenta);
    if (cuenta == 0) {
        Serial.println("    NO SE LEYO NADA del I2S: el microfono no responde.");
    }
}

// Alarma que suena al recibir una alerta. La genera ESTE firmware: por el aire
// solo viajo el aviso de 5 bytes.
//
// Bitono tipo sirena, con la fase acumulada en vez de recalculada: al cambiar
// de frecuencia la onda sigue donde estaba y no se escucha el "clic" del salto
// de fase.
static void tocarAlarma()
{
    Serial.println("[ALERTA] Sonando la alarma...");

    const bool veniaRecibiendo = audioReceiving;
    if (veniaRecibiendo) {
        // La alarma tiene prioridad sobre el audio que este entrando.
        audioReceiving = false;
        audioPlaying   = false;
        audioCerrando  = false;
    }

    audio.modoReproducir();

    const int N = BLOQUE_RX_MUESTRAS * 2;
    int16_t   buf[BLOQUE_RX_MUESTRAS * 2];

    const uint32_t totalMuestras = (uint32_t)SAMPLE_RATE * ALARMA_MS / 1000;
    const uint32_t porTramo      = (uint32_t)SAMPLE_RATE * ALARMA_TRAMO_MS / 1000;

    double   fase = 0.0;
    uint32_t n = 0;
    unsigned long t0 = millis();

    digitalWrite(LED_RX, HIGH);

    while (n < totalMuestras && (millis() - t0) < ALARMA_MS + 1000) {
        for (int i = 0; i < N; i++) {
            uint32_t hz = ((n / porTramo) & 1) ? ALARMA_HZ_B : ALARMA_HZ_A;
            buf[i] = (int16_t)(ALARMA_AMPL * sin(fase));
            fase += 2.0 * PI * (double)hz / SAMPLE_RATE;
            if (fase > 2.0 * PI) fase -= 2.0 * PI;   // acotar, no reiniciar
            n++;
        }

        int esc = 0;
        while (esc < N && (millis() - t0) < ALARMA_MS + 1000) {
            int w = audio.escribirParlante(&buf[esc], N - esc);
            if (w == 0) delay(1);
            esc += w;
            feedTheDog();
        }
    }

    audio.silencio();
    digitalWrite(LED_RX, LOW);
    FlushRX();
    Serial.println("[ALERTA] Fin de la alarma.");
}

// Manda el aviso al otro nodo. Son 5 bytes contra los 60 de un paquete de
// audio, asi que aguanta un enlace bastante peor: la alerta llega mas lejos
// que la voz.
static void enviarAlerta()
{
    uint8_t buf[5];
    buf[0] = MAGIC_BYTE;
    buf[1] = NODE_ID;
    buf[2] = PKT_KIND_ALERTA;
    buf[3] = (uint8_t)(alertasEnviadas & 0xFF);
    buf[4] = (uint8_t)((alertasEnviadas >> 8) & 0xFF);

    digitalWrite(LED_TX, HIGH);
    for (int i = 0; i < TX_REPEAT; i++) {
        rfEnviar(buf, sizeof(buf));
        delay(TX_REPEAT_MS);
    }
    digitalWrite(LED_TX, LOW);

    alertasEnviadas++;
    Serial.print(">> ALERTA enviada (#");
    Serial.print(alertasEnviadas);
    Serial.println(")");

    // Bip corto de confirmacion, distinto de la alarma de 2 s del otro lado.
    bip(BUZZ_FREQ_HZ, 80);
}

static void mostrarStats()
{
    Serial.println("--- Estadisticas ---");
    Serial.println(" PULSO:");
    Serial.print("   Enviados (esta placa): "); Serial.println(txSeq);
    Serial.print("   Recibidos validos:     "); Serial.println(rxTotal);
    Serial.print("   Huecos de secuencia:   "); Serial.println(rxPerdidos);
    if (rxHuboAlguno) {
        Serial.print("   Ultima seq recibida:   "); Serial.println(rxUltimaSeq);
        uint32_t esperados = rxUltimaSeq;
        if (esperados > 0) {
            float pct = 100.0f * rxTotal / esperados;
            Serial.print("   Tasa de exito aprox:   ");
            Serial.print(pct, 1);
            Serial.println(" %");
        }
    }
    Serial.println(" AUDIO:");
    Serial.print("   Paquetes recibidos:    "); Serial.println(audioRecibidos);
    Serial.print("   Paquetes perdidos:     "); Serial.println(audioPerdidos);
    if (audioRecibidos + audioPerdidos > 0) {
        float pct = 100.0f * audioPerdidos / (audioRecibidos + audioPerdidos);
        Serial.print("   Tasa de perdida:       ");
        Serial.print(pct, 2);
        Serial.println(" %");
    }
    // OJO con interpretar este numero: cuenta que el ring de recepcion se
    // quedo sin muestras, cosa NORMAL y sin consecuencias mientras el DMA del
    // I2S (32 ms) siga teniendo audio. El indicador real de si hubo cortes es
    // la relacion que imprime terminarSesionAudio(): audio reproducido contra
    // tiempo que tardo. Medido: 143 "underruns" con relacion 0.989x, o sea
    // reproduccion perfectamente continua.
    Serial.print("   Ring vacio (ver nota): "); Serial.println(audioUnderruns);
    Serial.print("   Timeouts de TX (RF):   "); Serial.println(rfTimeouts);
    Serial.print("   Veces que se uso el PTT: "); Serial.println(pttPulsaciones);
    Serial.print("   Alertas enviadas:      "); Serial.println(alertasEnviadas);
    Serial.print("   Alertas recibidas:     "); Serial.println(alertasRecibidas);
    Serial.println(" CAMINO DE RECEPCION:");
    Serial.print("   Syncs enganchados:     "); Serial.println(rxFlags);
    Serial.print("   Largo invalido:        "); Serial.println(rxLenMala);
    Serial.print("   CRC malo:              "); Serial.println(rxCrcMal);
    Serial.print("   MAGIC distinto:        "); Serial.println(rxMagicMal);
    Serial.print("   Paquete propio:        "); Serial.println(rxPropio);
    Serial.print("   Desbordes de FIFO:     "); Serial.println(rxOverflow);
    Serial.println("--------------------");
}

// ============================================================
//  AUDIO — ring de recepcion
// ============================================================
static inline void ringReset()
{
    rxRd = rxWr = rxN = 0;
}

static void ringPush(const int16_t* src, int n)
{
    for (int i = 0; i < n; i++) {
        if (rxN >= RX_RING_MUESTRAS) {
            // Lleno: tirar lo mas viejo. Pasa si el que transmite va mas
            // rapido de lo que el parlante consume; mejor perder audio viejo
            // que nuevo.
            rxRd = (uint16_t)((rxRd + 1) & (RX_RING_MUESTRAS - 1));
            rxN--;
        }
        rxRing[rxWr] = src[i];
        rxWr = (uint16_t)((rxWr + 1) & (RX_RING_MUESTRAS - 1));
        rxN++;
    }
}

static int ringPop(int16_t* dst, int maxN)
{
    int n = 0;
    while (n < maxN && rxN > 0) {
        dst[n++] = rxRing[rxRd];
        rxRd = (uint16_t)((rxRd + 1) & (RX_RING_MUESTRAS - 1));
        rxN--;
    }
    return n;
}

// ============================================================
//  AUDIO — transmision (streaming)
// ============================================================
static void enviarAudioHeader()
{
    uint8_t buf[4];
    buf[0] = MAGIC_BYTE;
    buf[1] = NODE_ID;
    buf[2] = PKT_KIND_AUDIO_HEADER;
    buf[3] = AUDIO_FMT_VERSION;   // el RX lo chequea: avisa si una placa
                                  // quedo con firmware viejo
    for (int i = 0; i < AUDIO_HEADER_REPEAT; i++) {
        rfEnviar(buf, sizeof(buf));
        delay(4);
    }
}

static void enviarAudioEnd()
{
    uint8_t buf[3];
    buf[0] = MAGIC_BYTE;
    buf[1] = NODE_ID;
    buf[2] = PKT_KIND_AUDIO_END;
    for (int i = 0; i < AUDIO_END_REPEAT; i++) {
        rfEnviar(buf, sizeof(buf));
        delay(4);
    }
}

// Graba y transmite al mismo tiempo. Sale cuando se suelta el pulsador (si
// 'usarPulsador') o cuando se cumple 'maxMs'.
// Si tonoHz != 0 manda un tono generado en vez de lo que capta el microfono.
// Sirve para probar la cadena completa (RF + decodificacion + parlante) con
// una senial conocida, sin depender de que el microfono ande.
static void transmitirAudioStreaming(unsigned long maxMs, bool usarPulsador,
                                     uint32_t tonoHz = 0)
{
    Serial.println();
    if (tonoHz) {
        Serial.print("=== TX TONO de ");
        Serial.print(tonoHz);
        Serial.println(" Hz por RF (sin microfono) ===");
    } else {
        Serial.println("=== TX audio (streaming) ===");
    }

    calibrarRF();
    audio.modoGrabar();
    dec.reset();

    AdpcmEstado st;                 // arranca en {0, 0}
    uint16_t    seq      = 0;
    uint32_t    enviados = 0;

    int16_t in16[BLOQUE_TX_MUESTRAS];
    // Acumulador a 8 kHz: junta lo que sale del decimador hasta completar un
    // paquete. Cota: se drena a <104 en cada vuelta, y cada vuelta agrega
    // como maximo BLOQUE_TX_MUESTRAS/2 + 1.
    int16_t pend[ADPCM_SAMPLES_PER_PKT + BLOQUE_TX_MUESTRAS];
    int     nPend = 0;
    uint8_t pkt[AUDIO_PKT_LEN];
    uint32_t faseTono = 0, muestrasTono = 0;

    digitalWrite(LED_TX, HIGH);
    enviarAudioHeader();

    unsigned long t0 = millis();
    while ((millis() - t0) < maxMs)
    {
        if (usarPulsador && digitalRead(BTN_PTT) != HIGH) break;   // solto

        if (tonoHz == 0) {
            int n = audio.leerMic(in16, BLOQUE_TX_MUESTRAS, 1);
            if (n > 0) {
                nPend += dec.procesar(in16, n, &pend[nPend]);
            }
        } else {
            // Generar el tono AL RITMO DEL RELOJ. Si se generara lo mas
            // rapido posible se mandaria mucho mas rapido que tiempo real y
            // al receptor se le desbordaria el buffer.
            const int tope = (int)(sizeof(pend) / sizeof(pend[0]));
            uint32_t deberia = (uint32_t)(((uint64_t)(millis() - t0) * CODEC_RATE) / 1000);
            while (muestrasTono < deberia && nPend < tope) {
                pend[nPend++] = (int16_t)(10000.0 *
                    sin(2.0 * PI * (double)faseTono * tonoHz / CODEC_RATE));
                faseTono++;
                muestrasTono++;
            }
            delay(1);
        }

        while (nPend >= ADPCM_SAMPLES_PER_PKT)
        {
            AdpcmEstado ini;
            adpcmCodificar(&st, pend, ADPCM_SAMPLES_PER_PKT,
                           &pkt[AUDIO_DATA_HDR_LEN], &ini);

            pkt[0] = MAGIC_BYTE;
            pkt[1] = NODE_ID;
            pkt[2] = PKT_KIND_AUDIO_DATA;
            pkt[3] =  seq       & 0xFF;
            pkt[4] = (seq >> 8) & 0xFF;
            pkt[5] = (uint8_t)( (uint16_t)ini.pred       & 0xFF);
            pkt[6] = (uint8_t)(((uint16_t)ini.pred >> 8) & 0xFF);
            pkt[7] = (uint8_t)ini.index;
            seq++;

            rfEnviar(pkt, sizeof(pkt));
            enviados++;

            nPend -= ADPCM_SAMPLES_PER_PKT;
            if (nPend > 0) {
                memmove(pend, pend + ADPCM_SAMPLES_PER_PKT,
                        (size_t)nPend * sizeof(int16_t));
            }

#if AUDIO_INTERPACKET_DELAY_MS > 0
            delay(AUDIO_INTERPACKET_DELAY_MS);
#endif
        }

        feedTheDog();
    }

    enviarAudioEnd();
    digitalWrite(LED_TX, LOW);

    // Bip corto al terminar: confirma "transmiti" sin necesidad de monitor
    // serie. Va al SOLTAR y no al apretar, asi no se graba dentro del audio.
    bip(BUZZ_FREQ_HZ, 60);

    unsigned long dt = millis() - t0;
    Serial.print(">> ");
    Serial.print(enviados);
    Serial.print(" paquetes en ");
    Serial.print(dt / 1000.0, 2);
    Serial.print(" s (");
    Serial.print(enviados * (1000.0 * ADPCM_SAMPLES_PER_PKT / CODEC_RATE) / (dt ? dt : 1), 2);
    Serial.println("x tiempo real; 1.00 = justo)");
}

// ============================================================
//  AUDIO — recepcion (streaming)
// ============================================================
static void iniciarSesionAudio()
{
    ringReset();
    interp.reset();

    outStageN = outStageRd = 0;

    capturadas         = 0;
    audioReceiving     = true;
    audioPlaying       = false;
    audioCerrando      = false;
    enUnderrun         = false;
    audioUltimoPaquete = millis();

    Serial.println();
    Serial.println("[AUDIO] Sesion nueva (streaming)");
}

static void terminarSesionAudio()
{
    Serial.print("[AUDIO] Fin | recibidos=");
    Serial.print(audioRecibidos);
    Serial.print(" perdidos=");
    Serial.print(audioPerdidos);
    Serial.print(" underruns=");
    Serial.println(audioUnderruns);

    if (muestrasSalida > 0) {
        float dura = (float)muestrasSalida / CODEC_RATE;         // lo que dura el audio
        float tardo = (millis() - playT0) / 1000.0f;             // lo que tardo en salir
        Serial.print("[AUDIO] Reproducidas ");
        Serial.print(muestrasSalida);
        Serial.print(" muestras = ");
        Serial.print(dura, 2);
        Serial.print(" s de audio, en ");
        Serial.print(tardo, 2);
        Serial.print(" s reales (");
        Serial.print(dura > 0 ? tardo / dura : 0.0f, 3);
        Serial.println("x; 1.00 = sin cortes)");

        double rmsOut = muestrasSalida ? sqrt((double)sumaSalida / muestrasSalida) : 0.0;
        Serial.print("[AUDIO] Nivel reproducido: pico=");
        Serial.print(picoSalida);
        Serial.print(" (");
        Serial.print(picoSalida > 0 ? 20.0 * log10((double)picoSalida / 32768.0) : -99.0, 1);
        Serial.print(" dBFS)  RMS=");
        Serial.print(rmsOut, 0);
        if (picoSalida == 0) {
            Serial.println("  <-- LLEGA SILENCIO: el mic del que transmite no capta nada");
        } else {
            Serial.println("  <-- hay audio: si no se escucha, es el amplificador o el parlante");
        }

        // Dibujar lo que realmente llego, muestra por muestra.
        if (capturadas > 0) {
            Serial.print("[AUDIO] Buffer recibido (primeras ");
            Serial.print(capturadas);
            Serial.println(" muestras a 8 kHz):");
            for (uint8_t i = 0; i < capturadas; i++) {
                int col = 30 + (int)((long)captura[i] * 28 / 32768);
                Serial.print("  ");
                if (captura[i] >= 0) Serial.print(" ");
                Serial.print(captura[i]);
                Serial.print("	|");
                for (int c = 0; c < 60; c++) {
                    Serial.print(c == col ? '*' : (c == 30 ? '|' : ' '));
                }
                Serial.println();
            }
        }
    }

    audioReceiving = false;
    audioPlaying   = false;
    audioCerrando  = false;
    audio.silencio();
    digitalWrite(LED_RX, LOW);
}

// Mueve audio del ring al parlante. Se llama en cada vuelta del lazo y NO
// bloquea: si el DMA esta lleno se vuelve y se reintenta despues.
static void servirReproduccion()
{
    if (!audioReceiving) return;

    // Arrancar recien cuando hay suficiente juntado (jitter buffer), o si ya
    // se esta cerrando la sesion y hay que drenar lo que quedo.
    if (!audioPlaying) {
        if (rxN >= JITTER_PREBUFFER || (audioCerrando && rxN > 0)) {
            audio.modoReproducir();
            interp.reset();
            audioPlaying   = true;
            playT0         = millis();
            muestrasSalida = 0;
            picoSalida     = 0;
            sumaSalida     = 0;
            digitalWrite(LED_RX, HIGH);
        } else if (audioCerrando) {
            terminarSesionAudio();
        }
        return;
    }

    // 1) Vaciar lo que quedo pendiente del intento anterior.
    if (outStageRd < outStageN) {
        int w = audio.escribirParlante(&outStage[outStageRd],
                                       outStageN - outStageRd);
        outStageRd += w;
        if (outStageRd < outStageN) return;      // sigue lleno, despues
        outStageN = outStageRd = 0;
    }

    // 2) Generar el proximo bloque.
    int16_t tmp[BLOQUE_RX_MUESTRAS];
    int n = ringPop(tmp, BLOQUE_RX_MUESTRAS);

    if (n == 0) {
        if (audioCerrando) {
            terminarSesionAudio();
            return;
        }
        // Underrun: el ring se vacio pero la sesion sigue abierta.
        //
        // A proposito NO metemos silencio a mano: el driver esta con
        // tx_desc_auto_clear, asi que un hueco en el DMA ya sale como
        // silencio. Rellenar nosotros agregaria muestras de mas al stream, y
        // esa latencia no se recupera nunca (cada underrun correria el audio
        // un poco mas atras).
        //
        // Se cuenta solo al ENTRAR en underrun: esta funcion se llama miles
        // de veces por segundo y si no el numero no significa nada.
        if (!enUnderrun) {
            audioUnderruns++;
            enUnderrun = true;
        }
        return;
    }
    enUnderrun = false;
    muestrasSalida += n;

    // Nivel de lo que de verdad se esta reproduciendo. Separa "no llega
    // audio" de "llega audio pero no suena el parlante": si el pico queda en
    // 0, el que transmite esta mandando silencio (mic mudo); si hay nivel y
    // igual no se escucha, el problema es el amplificador o el parlante.
    for (int i = 0; i < n; i++) {
        int32_t a = tmp[i] < 0 ? -(int32_t)tmp[i] : tmp[i];
        if (a > (int32_t)picoSalida) picoSalida = (uint16_t)a;
        sumaSalida += (uint64_t)((int32_t)tmp[i] * (int32_t)tmp[i]);
    }

    outStageN  = interp.procesar(tmp, n, outStage);
    outStageRd = 0;
}

// ============================================================
//  MONITOR LOCAL ('m')
// ============================================================
// Graba, pasa por FIR + ADPCM y reproduce, todo sin tocar la radio. Sirve
// para separar "el audio suena mal" de "el enlace anda mal": si por aca
// suena bien, el problema es RF.
static void monitorLocal()
{
    Serial.println();
    Serial.println("=== Monitor local (mic -> FIR -> ADPCM -> FIR -> parlante) ===");

    int16_t* mon = audio.monitor();
    const int cap = audio.monitorMuestras();

    audio.modoGrabar();
    dec.reset();

    int n = 0;
    int16_t in16[BLOQUE_TX_MUESTRAS];
    unsigned long t0 = millis();

    // Cortar con margen para que el decimador nunca escriba pasado 'cap'.
    while (n + (BLOQUE_TX_MUESTRAS / 2 + 1) <= cap && (millis() - t0) < 4000) {
        int r = audio.leerMic(in16, BLOQUE_TX_MUESTRAS, 1);
        if (r > 0) n += dec.procesar(in16, r, &mon[n]);
        feedTheDog();
    }
    Serial.print("Grabadas ");
    Serial.print(n);
    Serial.print(" muestras a ");
    Serial.print(CODEC_RATE);
    Serial.println(" Hz");

    // Medir el nivel capturado. Es lo que dice si MIC_SHIFT esta bien puesto:
    //   pico cerca de 0 dBFS  -> se esta recortando, SUBIR MIC_SHIFT
    //   pico por debajo de -20 dBFS -> sobra margen, BAJAR MIC_SHIFT
    // Lo comodo para voz es pico entre -12 y -3 dBFS.
    {
        int32_t pico = 0;
        double  suma = 0.0;
        int32_t recortadas = 0;
        for (int i = 0; i < n; i++) {
            int32_t a = mon[i] < 0 ? -(int32_t)mon[i] : mon[i];
            if (a > pico) pico = a;
            if (a >= 32767) recortadas++;
            suma += (double)mon[i] * (double)mon[i];
        }
        double rms = n > 0 ? sqrt(suma / n) : 0.0;

        Serial.print("Nivel: pico=");
        Serial.print(pico);
        Serial.print(" (");
        Serial.print(pico > 0 ? 20.0 * log10((double)pico / 32768.0) : -99.0, 1);
        Serial.print(" dBFS)  RMS=");
        Serial.print(rms, 0);
        Serial.print(" (");
        Serial.print(rms > 0 ? 20.0 * log10(rms / 32768.0) : -99.0, 1);
        Serial.print(" dBFS)  recortadas=");
        Serial.print(recortadas);
        Serial.print("  MIC_SHIFT=");
        Serial.println(MIC_SHIFT);
    }

    // Ida y vuelta por el codec, en bloques autocontenidos igual que por RF.
    AdpcmEstado st;
    uint8_t comp[ADPCM_BYTES_PER_PKT];
    for (int off = 0; off + ADPCM_SAMPLES_PER_PKT <= n;
         off += ADPCM_SAMPLES_PER_PKT) {
        AdpcmEstado ini;
        adpcmCodificar(&st, &mon[off], ADPCM_SAMPLES_PER_PKT, comp, &ini);
        adpcmDecodificar(&ini, comp, ADPCM_BYTES_PER_PKT, &mon[off]);
        feedTheDog();
    }

    Serial.println("Reproduciendo...");
    audio.modoReproducir();
    interp.reset();

    int16_t sal[BLOQUE_RX_MUESTRAS * 2];
    int off = 0;
    while (off < n) {
        int blo = (n - off > BLOQUE_RX_MUESTRAS) ? BLOQUE_RX_MUESTRAS : (n - off);
        int m = interp.procesar(&mon[off], blo, sal);

        int escrito = 0;
        while (escrito < m) {
            int w = audio.escribirParlante(&sal[escrito], m - escrito);
            if (w == 0) delay(1);
            escrito += w;
            feedTheDog();
        }
        off += blo;
    }

    audio.silencio();
    Serial.println("Fin del monitor.");
}

// ============================================================
//  SETUP
// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println("=========================================");
    Serial.println("  ENLACE CC1101 + VOZ (streaming ADPCM)");
    Serial.print(  "  NODO ");
    Serial.println(NODE_ID);
    Serial.println("=========================================");

    pinMode(BTN_PTT,    INPUT_PULLDOWN);
    pinMode(BTN_ALERTA, INPUT_PULLDOWN);
    pinMode(LED_TX,  OUTPUT);
    pinMode(LED_RX,  OUTPUT);
    digitalWrite(LED_TX, LOW);
    digitalWrite(LED_RX, LOW);

    // Suscribir loopTask al task watchdog (ver feedTheDog). Si el WDT no esta
    // inicializado esto falla sin drama y feedTheDog() queda como no-op.
    wdtSuscrito = (esp_task_wdt_add(NULL) == ESP_OK);
    Serial.print("[WDT] loopTask suscrita: ");
    Serial.println(wdtSuscrito ? "si" : "no");

    buzzerInit();

    if (!audio.begin()) {
        Serial.println("!! Fallo el audio (I2S). Freno.");
        while (1) { delay(1000); feedTheDog(); }
    }

    if (!initCC1101()) {
        Serial.println("!! El CC1101 no responde. Revisar cableado SPI.");
        bip(400, 800);
    }

    Serial.print("[AUDIO] codec ");
    Serial.print(CODEC_RATE);
    Serial.print(" Hz, ADPCM 4 bits = ");
    Serial.print(CODEC_RATE * 4 / 1000);
    Serial.print(" kbps | ");
    Serial.print(ADPCM_SAMPLES_PER_PKT);
    Serial.print(" muestras/paquete = ");
    Serial.print(1000.0 * ADPCM_SAMPLES_PER_PKT / CODEC_RATE, 1);
    Serial.println(" ms");
    Serial.print("[AUDIO] jitter buffer ");
    Serial.print(1000.0 * JITTER_PREBUFFER / CODEC_RATE, 0);
    Serial.println(" ms");

    FlushRX();
    bip(BUZZ_FREQ_HZ, 120);

    Serial.println();
    Serial.println("[RX] Escuchando...");
    Serial.print("Pulsador PTT (GPIO");
    Serial.print(BTN_PTT);
    Serial.println(") = hablar mientras este apretado");
    Serial.print("Pulsador ALERTA (GPIO");
    Serial.print(BTN_ALERTA);
    Serial.println(") = hace sonar la alarma en el otro nodo");
    Serial.println("'k'=PTT manda tono de 1 kHz / 2 s en vez del microfono (probar enlace sin mic)");
    Serial.println("'a'=transmitir 3 s | 't'=pulso | 'm'=monitor local (sin RF)");
    Serial.println("'b'=baliza | 'z'=buzzer | 's'=stats | 'c'=reset");
    Serial.println("ALERTA: 'l'=mandar alerta al otro | 'v'=probar la alarma local");
    Serial.println("Diagnostico audio: 'o'=tono local | 'j'=tono por RF | 'n'=medidor de mic | 'u'=barrido BCLK");
    Serial.println("Diagnostico RF: 'r'=registros | 'x'=RSSI | 'g'=CS/SYNC | 'd'=tramas | 'w'=potencia TX");
    Serial.println("ALCANCE: 'e'=emisor (la placa que camina) | 'p'=medidor (la de la notebook)");
    Serial.print("OJO: a ");
    Serial.print(RF_PA_DBM);
    Serial.println(" dBm con las placas juntas el receptor se satura y no recibe nada: bajar con 'w'.");
}

// ============================================================
//  LOOP
// ============================================================
void loop()
{
    char cmd = 0;
    if (Serial.available()) {
        cmd = Serial.read();
    }

    // ---------- Pulsador: flanco de subida arranca el PTT ----------
    bool reading = digitalRead(BTN_PTT);
    if (reading != btnLast) {
        btnLastTime = millis();
    }
    if ((millis() - btnLastTime) > DEBOUNCE_MS) {
        if (reading != btnState) {
            btnState = reading;
            if (btnState == HIGH) {
                cmd = 'P';
            }
        }
    }
    btnLast = reading;

    // ---------- Segundo pulsador: alerta ----------
    bool leeAlt = digitalRead(BTN_ALERTA);
    if (leeAlt != altLast) {
        altLastTime = millis();
    }
    if ((millis() - altLastTime) > DEBOUNCE_MS) {
        if (leeAlt != altState) {
            altState = leeAlt;
            if (altState == HIGH) {
                cmd = 'L';          // flanco de subida: mandar alerta
            }
        }
    }
    altLast = leeAlt;

    // Es half-duplex: no se puede transmitir mientras se esta recibiendo.
    // La alerta ('L') queda fuera de este guard a proposito: es prioritaria.
    if (audioReceiving && (cmd == 'P' || cmd == 'a' || cmd == 'A' ||
                           cmd == 't' || cmd == 'T')) {
        Serial.println(">> Ocupado recibiendo audio, espera.");
        cmd = 0;
    }

    switch (cmd) {
        case 'L':
        case 'l':
            enviarAlerta();
            break;

        case 'v':
        case 'V':
            tocarAlarma();          // probar el sonido sin usar la radio
            break;

        case 'P':
            pttPulsaciones++;
            if (pttTono) {
                // Tono de duracion fija: no se corta al soltar, asi un toque
                // corto manda igual los 2 s completos.
                transmitirAudioStreaming(PTT_TONO_MS, false, pttTonoHz);
            } else {
                transmitirAudioStreaming(PTT_MAX_MS, true);
            }
            break;

        case 'k':
        case 'K':
            pttTono = !pttTono;
            Serial.println();
            if (pttTono) {
                Serial.print(">> PTT en MODO TONO: el pulsador manda ");
                Serial.print(pttTonoHz);
                Serial.print(" Hz durante ");
                Serial.print(PTT_TONO_MS);
                Serial.println(" ms por RF, sin usar el microfono.");
            } else {
                Serial.println(">> PTT en modo VOZ: el pulsador transmite el microfono.");
            }
            break;

        case 'a':
        case 'A':
            transmitirAudioStreaming(TX_SERIAL_MS, false);
            break;

        case 't':
        case 'T':
            transmitirPulso();
            break;

        case 'm':
        case 'M':
            monitorLocal();
            break;

        case 'b':
        case 'B':
            beaconOn = !beaconOn;
            Serial.print(">> Modo BALIZA: ");
            Serial.println(beaconOn ? "ON (manda solo cada 1 s)" : "OFF");
            beaconLast = millis();
            break;

        // Emisor de la prueba de alcance: esta placa es la que CAMINA.
        case 'e':
        case 'E':
            alcEmisor = !alcEmisor;
            Serial.println();
            if (alcEmisor) {
                alcEmisorLast = millis();
                Serial.println("=== ALCANCE / EMISOR: ON ===");
                Serial.print("Manda ");
                Serial.print(1000UL / ALCANCE_PERIOD_MS);
                Serial.println(" paquetes/s, uno por secuencia, sin repetir.");
                Serial.println("Esta placa es la que CAMINA. No mide: mide la otra.");
                if (alcMedidor)
                    Serial.println("OJO: el medidor tambien esta ON en esta placa y se van a tapar.");
            } else {
                Serial.println("=== ALCANCE / EMISOR: OFF ===");
            }
            break;

        // Medidor: esta placa queda FIJA en la notebook. Solo minuscula, que la
        // 'P' mayuscula es la marca interna del pulsador y ya tiene su case.
        case 'p':
            alcMedidor = !alcMedidor;
            Serial.println();
            if (alcMedidor) {
                alcanceResetVentana();
                alcVentana     = 0;
                alcInformeLast = millis();
                Serial.println("=== ALCANCE / MEDIDOR: ON ===");
                Serial.print("Informa una linea cada ");
                Serial.print(ALCANCE_INFORME_MS / 1000UL);
                Serial.println(" s, aunque no llegue nada.");
                Serial.println("Esta placa queda FIJA en la notebook; la otra camina con 'e'.");
                Serial.println();
                // FSPL = 32.44 + 20*log10(f_MHz) + 20*log10(d_km), con 10 dBm
                // de emision y antenas de 0 dBi. Es el techo teorico: lo que
                // se mida por debajo es perdida de antena y frente de RF.
                Serial.println("Referencia en espacio libre (10 dBm, antenas 0 dBi):");
                Serial.println("   10 m -> -35 dBm | 30 m -> -45 dBm");
                Serial.println("  100 m -> -55 dBm | 200 m -> -61 dBm");
                Serial.println("Sensibilidad del CC1101 a ~100 kbps: del orden de -90 dBm.");
                Serial.println("LQI: mas BAJO es mejor (es una estimacion de error).");
                if (alcEmisor)
                    Serial.println("OJO: el emisor tambien esta ON en esta placa y se van a tapar.");
            } else {
                alcanceInforme();                 // cierra la ventana en curso
                Serial.println("=== ALCANCE / MEDIDOR: OFF ===");
            }
            break;

        case 'z':
        case 'Z':
            Serial.println(">> Probando buzzer local...");
            bip(BUZZ_FREQ_HZ, BUZZ_MS);
            break;

        case 's':
        case 'S':
            mostrarStats();
            break;

        case 'r':
        case 'R':
            volcarRegistrosRF();
            break;

        case 'd':
        case 'D':
            rxVerboso = !rxVerboso;
            Serial.print(">> Volcado de tramas recibidas: ");
            Serial.println(rxVerboso ? "ON" : "OFF");
            break;

        case 'w':
        case 'W': {
            // Barrido de potencia de TX. Si el receptor esta saturado por
            // tenerlo al lado, bajar potencia HACE QUE EMPIECE a andar.
            static const int niveles[] = { -30, -20, -10, 0, 5, 7, 10 };
            static uint8_t idx = 0;
            idx = (uint8_t)((idx + 1) % (sizeof(niveles)/sizeof(niveles[0])));
            ELECHOUSE_cc1101.setPA(niveles[idx]);
            calibrarRF();
            Serial.print(">> Potencia TX = ");
            Serial.print(niveles[idx]);
            Serial.println(" dBm");
            break;
        }

        case 'j':
        case 'J':
            transmitirAudioStreaming(3000, false, 1000);
            break;

        case 'n':
        case 'N':
            medidorMic(12000);
            break;

        case 'u':
        case 'U':
            barridoBclk();
            break;

        case 'o':
        case 'O':
            tonoPrueba(1000, 3000, 12000);     // 1 kHz, 3 s, -8.7 dBFS
            break;

        case 'g':
        case 'G':
            monitorGdo(5000);
            break;

        case 'x':
        case 'X':
            monitorRssi(5000);
            break;

        case 'c':
        case 'C':
            txSeq = 0; rxTotal = 0; rxPerdidos = 0;
            rxUltimaSeq = 0; rxHuboAlguno = false;
            audioRecibidos = 0; audioPerdidos = 0; audioUnderruns = 0;
            rfTimeouts = 0;
            rxFlags = 0; rxLenMala = 0; rxCrcMal = 0;
            rxMagicMal = 0; rxPropio = 0; rxOverflow = 0;
            pttPulsaciones = 0;
            alertasEnviadas = 0; alertasRecibidas = 0;
            Serial.println(">> Contadores reseteados.");
            break;

        default:
            break;
    }

    if (beaconOn && !audioReceiving &&
        (millis() - beaconLast >= BEACON_PERIOD_MS)) {
        beaconLast = millis();
        transmitirPulso();
    }

    // Emisor de alcance: un paquete por seq (sin repetir, para no esconder la
    // perdida) y sin imprimir, que a 5/s llenaria la pantalla.
    if (alcEmisor && !audioReceiving &&
        (millis() - alcEmisorLast >= ALCANCE_PERIOD_MS)) {
        alcEmisorLast = millis();
        transmitirPulso(false, false);
    }

    // Medidor de alcance: informa por tiempo, llegue o no llegue algo.
    if (alcMedidor && (millis() - alcInformeLast >= ALCANCE_INFORME_MS)) {
        alcInformeLast = millis();
        alcanceInforme();
    }

    // ---------- Recepcion ----------
    // No se usa CheckReceiveFlag() de la libreria: esa detecta el FLANCO de
    // GDO0 y se queda esperando a que baje. Con el audio en streaming llegan
    // paquetes cada 13 ms y es facil perderse un flanco; el paquete queda sin
    // leer en el FIFO, entra el siguiente, el FIFO (64 B) se desborda y el
    // chip se planta en RXFIFO_OVERFLOW, donde deja de recibir PARA SIEMPRE
    // hasta que se lo limpie. Medido: enganchaba 27 de 231 paquetes y cortaba.
    //
    // En su lugar se mira el FIFO directamente. GDO0 en BAJO significa que no
    // hay recepcion en curso, asi que si ademas hay bytes, lo que esta ahi es
    // un paquete COMPLETO y se puede leer sin carrera.
    uint8_t rxBytes = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);

    if (rxBytes & 0x80) {                 // bit 7 = desborde
        rxOverflow++;
        FlushRX();
    }
    else if ((rxBytes & 0x7F) > 0 && digitalRead(PIN_GDO0) == LOW)
    {
        rxFlags++;

        uint8_t buf[64] = {0};
        uint8_t len = 0, lqi = 0;
        bool    crcOk = false;
        int     rssi  = 0;

        bool hayTrama = rfRecibir(buf, sizeof(buf), &len, &crcOk, &rssi, &lqi);

        if (rxVerboso) {
            Serial.print("<RX> len=");
            Serial.print(len);
            Serial.print(" crc=");
            Serial.print(crcOk ? "ok " : "MAL");
            Serial.print(" rssi=");
            Serial.print(rssi);
            Serial.print(" lqi=");
            Serial.print(lqi);
            Serial.print(" |");
            for (int i = 0; i < len && i < 8; i++) {
                Serial.print(" ");
                if (buf[i] < 16) Serial.print("0");
                Serial.print(buf[i], HEX);
            }
            Serial.println();
        }

        // El orden de los descartes importa para que los contadores digan la
        // verdad: primero CRC, despues largo, despues contenido.
        if (!hayTrama) {
            rxLenMala++;
        }
        else if (!crcOk) {
            rxCrcMal++;
            FlushRX();
        }
        else if (len > AUDIO_PKT_LEN) {
            rxLenMala++;
            FlushRX();
        }
        else if (len < 3 || buf[0] != MAGIC_BYTE) {
            rxMagicMal++;
        }
        else if (buf[1] == NODE_ID) {
            rxPropio++;
        }
        else
        {
            uint8_t kind = buf[2];

            if (kind == PKT_KIND_PULSE && len >= 5)
            {
                uint16_t seq = buf[3] | ((uint16_t)buf[4] << 8);

                if (!rxHuboAlguno || seq != rxUltimaSeq)
                {
                    // rssi y lqi vienen de los bytes de estado del propio
                    // paquete, no de los registros: los registros ya los piso
                    // la recepcion siguiente.

                    if (rxHuboAlguno && seq > rxUltimaSeq + 1) {
                        rxPerdidos += (seq - rxUltimaSeq - 1);
                    }

                    rxUltimaSeq  = seq;
                    rxHuboAlguno = true;
                    rxTotal++;

                    // En prueba de alcance se acumula y se informa por
                    // ventana, sin linea por paquete y sin bip: el bip
                    // bloquea 300 ms y a 200 ms de periodo perderia la mitad.
                    if (alcMedidor) {
                        if (alcRecibidos == 0) {
                            alcRssiMin = alcRssiMax = rssi;
                            alcLqiPeor = lqi;
                        } else {
                            if (rssi < alcRssiMin) alcRssiMin = rssi;
                            if (rssi > alcRssiMax) alcRssiMax = rssi;
                            if (lqi  > alcLqiPeor) alcLqiPeor = lqi;
                        }
                        alcRssiSuma += rssi;
                        alcLqiSuma  += lqi;
                        alcRecibidos++;

                        digitalWrite(LED_RX, HIGH);
                        delay(2);
                        digitalWrite(LED_RX, LOW);
                    }
                    else
                    {
                        Serial.print("<< PULSO de nodo ");
                        Serial.print(buf[1]);
                        Serial.print(" | seq=");
                        Serial.print(seq);
                        Serial.print(" | RSSI=");
                        Serial.print(rssi);
                        Serial.print(" dBm | LQI=");
                        Serial.println(lqi);

                        // El bip bloquea BUZZ_MS (300 ms). Si justo esta
                        // entrando audio, ese parate vacia el DMA y se
                        // escucha: en ese caso solo el LED.
                        digitalWrite(LED_RX, HIGH);
                        if (!audioReceiving) bip(BUZZ_FREQ_HZ, BUZZ_MS);
                        digitalWrite(LED_RX, LOW);
                    }
                }
            }
            else if (kind == PKT_KIND_AUDIO_HEADER && len >= 4)
            {
                if (buf[3] != AUDIO_FMT_VERSION) {
                    Serial.print("[AUDIO] Formato incompatible (recibi v");
                    Serial.print(buf[3]);
                    Serial.print(", soy v");
                    Serial.print(AUDIO_FMT_VERSION);
                    Serial.println("). Reflashear las dos placas.");
                }
                else if (!audioReceiving) {
                    audioSeqEsperada = 0;
                    iniciarSesionAudio();
                }
            }
            else if (kind == PKT_KIND_AUDIO_DATA && len == AUDIO_PKT_LEN)
            {
                // Si se perdio el header, abrir la sesion con el primer dato
                // que llegue: mejor que quedarse mudo toda la transmision.
                if (!audioReceiving) {
                    audioSeqEsperada = buf[3] | ((uint16_t)buf[4] << 8);
                    iniciarSesionAudio();
                }

                uint16_t seq = buf[3] | ((uint16_t)buf[4] << 8);

                AdpcmEstado st;
                st.pred  = (int16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8));
                // El indice viene del aire: acotarlo antes de usarlo como
                // subindice de la tabla de pasos.
                int idx = (int)(int8_t)buf[7];
                if (idx <  0) idx =  0;
                if (idx > 88) idx = 88;
                st.index = (int8_t)idx;

                uint16_t hueco = (uint16_t)(seq - audioSeqEsperada);
                if (hueco > 0 && hueco < 1000) audioPerdidos += hueco;
                audioSeqEsperada = (uint16_t)(seq + 1);

                int16_t dec8[ADPCM_SAMPLES_PER_PKT];
                adpcmDecodificar(&st, &buf[AUDIO_DATA_HDR_LEN],
                                 ADPCM_BYTES_PER_PKT, dec8);
                // guardar el arranque de la sesion para mostrarlo al final
                for (int i = 0; i < ADPCM_SAMPLES_PER_PKT && capturadas < CAPTURA_N; i++) {
                    captura[capturadas++] = dec8[i];
                }

                ringPush(dec8, ADPCM_SAMPLES_PER_PKT);

                audioRecibidos++;
                audioUltimoPaquete = millis();
            }
            else if (kind == PKT_KIND_ALERTA && len >= 3)
            {
                uint16_t idAlerta = (len >= 5)
                                  ? (buf[3] | ((uint16_t)buf[4] << 8)) : 0;
                // Llega repetida TX_REPEAT veces: sonar una sola vez.
                static uint16_t ultimaAlerta = 0xFFFF;
                static unsigned long ultimaAlertaMs = 0;
                if (idAlerta != ultimaAlerta || (millis() - ultimaAlertaMs) > 3000) {
                    ultimaAlerta   = idAlerta;
                    ultimaAlertaMs = millis();
                    alertasRecibidas++;
                    Serial.print("<< ALERTA del nodo ");
                    Serial.print(buf[1]);
                    Serial.print(" | RSSI=");
                    Serial.print(rssi);
                    Serial.print(" dBm | LQI=");
                    Serial.println(lqi);
                    tocarAlarma();
                }
            }
            else if (kind == PKT_KIND_AUDIO_END && audioReceiving)
            {
                audioCerrando = true;
            }
        }
        // Sin FlushRX() aca a proposito: ReceiveData() ya deja el chip
        // escuchando, y con RXOFF_MODE=RX no hace falta pasar por IDLE. El
        // FlushRX() que habia costaba una recalibracion por paquete.
    }

    // Si dejaron de llegar paquetes, el otro solto el PTT.
    if (audioReceiving && !audioCerrando &&
        (millis() - audioUltimoPaquete > AUDIO_RX_TIMEOUT_MS))
    {
        audioCerrando = true;
    }

    servirReproduccion();

    // Imprescindible: loopTask esta suscrita al task watchdog (ver setup), asi
    // que si el lazo no lo alimenta la placa se resetea sola a los 5 s.
    feedTheDog();
}
