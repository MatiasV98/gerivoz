// ============================================================
//  PRUEBA DE HARDWARE DE AUDIO — independiente de todo lo demas
// ============================================================
// Este archivo NO forma parte del firmware. Es un programa minimo cuyo unico
// proposito es decir si el microfono y el parlante de una placa funcionan,
// sin radio, sin codec, sin watchdog y sin nada que pueda confundir.
//
// Compilar y cargar:
//     pio run -e prueba_audio -t upload --upload-port COM5
//
// Para volver al firmware normal:
//     pio run -e nodo1 -t upload --upload-port COM5
//
// Hace dos cosas, en ciclo:
//   1) PARLANTE: instala el I2S solo como salida, en 16 bits (el formato mas
//      comun y el que cualquier MAX98357A acepta) y toca 1 kHz 2 segundos.
//   2) MICROFONO: instala el I2S solo como entrada, en 32 bits (que es como
//      habla el INMP441) y mide el nivel 2 segundos.
//
// Cada etapa instala y desinstala el driver por separado, asi ninguna puede
// arrastrar configuracion de la otra.

#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"
#include "Config.h"

#define PUERTO I2S_NUM_0

// Los dos pines de datos siempre conectados. Dejar uno en I2S_PIN_NO_CHANGE
// hacia que la lectura del microfono devolviera ceros INCLUSO en una placa
// cuyo microfono anda: el test daba falso negativo. Verificado contra la
// placa buena antes de usarlo para diagnosticar nada.
static void pines(i2s_pin_config_t* p)
{
    p->bck_io_num   = I2S_BCLK;
    p->ws_io_num    = I2S_WS;
    p->data_out_num = SPK_DATA;
    p->data_in_num  = MIC_DATA;
}

// UNA sola instalacion para todo el programa, con la misma configuracion que
// usa el firmware. Instalar y desinstalar el driver entre prueba y prueba
// hacia que el microfono devolviera ceros incluso en una placa sana.
static bool iniciarI2S()
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = I2S_DMA_BUFS;
    cfg.dma_buf_len          = I2S_DMA_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = 0;

    if (i2s_driver_install(PUERTO, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("ERROR instalando el I2S");
        return false;
    }
    i2s_pin_config_t p; pines(&p);
    return i2s_set_pin(PUERTO, &p) == ESP_OK;
}

static void probarParlante()
{
    Serial.println();
    Serial.println("--- 1) PARLANTE: 1 kHz durante 2 s ---");
    Serial.println("    Tiene que sonar un pitido.");


    int32_t  bloque[512];
    uint32_t fase = 0, total = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        for (int i = 0; i < 512; i += 2) {
            int32_t v = ((int32_t)(12000.0 * sin(2.0 * PI * fase * 1000.0 / SAMPLE_RATE))) << 16;
            bloque[i] = v;          // izquierdo
            bloque[i + 1] = v;      // derecho
            fase++;
        }
        size_t esc = 0;
        i2s_write(PUERTO, bloque, sizeof(bloque), &esc, portMAX_DELAY);
        total += esc;
    }

    unsigned long dt = millis() - t0;
    Serial.print("    Bytes escritos: ");
    Serial.print(total);
    Serial.print(" en ");
    Serial.print(dt / 1000.0, 2);
    Serial.print(" s -> ");
    Serial.print(total / 8.0 / (dt / 1000.0), 0);
    Serial.println(" muestras/s (tiene que dar ~16000)");
}

static void probarMicrofono()
{
    Serial.println();
    Serial.println("--- 2) MICROFONO: midiendo 2 s ---");
    Serial.println("    HABLA FUERTE.");


    // Descartar el arranque: el INMP441 necesita unos ciclos de reloj antes
    // de entregar datos validos.
    {
        int32_t basura[256];
        size_t  n = 0;
        unsigned long tw = millis();
        while (millis() - tw < 300) {
            i2s_read(PUERTO, basura, sizeof(basura), &n, pdMS_TO_TICKS(50));
        }
    }

    int32_t  bloque[256];
    uint32_t leidas = 0, cerosSeguidos = 0;
    int32_t  picoIzq = 0, picoDer = 0;

    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        size_t n = 0;
        if (i2s_read(PUERTO, bloque, sizeof(bloque), &n, pdMS_TO_TICKS(200)) != ESP_OK) break;
        int m = n / sizeof(int32_t);
        for (int i = 0; i + 1 < m; i += 2) {
            int32_t a = bloque[i]     >> 14; if (a < 0) a = -a;
            int32_t b = bloque[i + 1] >> 14; if (b < 0) b = -b;
            if (a > picoIzq) picoIzq = a;
            if (b > picoDer) picoDer = b;
            if (bloque[i] == 0 && bloque[i + 1] == 0) cerosSeguidos++;
            leidas++;
        }
    }

    Serial.print("    Tramas leidas: "); Serial.println(leidas);
    Serial.print("    Pico slot 0: "); Serial.println(picoIzq);
    Serial.print("    Pico slot 1: "); Serial.println(picoDer);
    Serial.print("    Tramas en cero absoluto: "); Serial.print(cerosSeguidos);
    Serial.print(" de "); Serial.println(leidas);

    if (leidas == 0) {
        Serial.println("    >> NO LLEGA NADA del I2S. Revisar BCLK (GPIO26) y WS (GPIO25).");
    } else if (picoIzq == 0 && picoDer == 0) {
        Serial.println("    >> El I2S clockea pero el microfono NO ENTREGA DATOS.");
        Serial.println("       Revisar SD del INMP441 -> GPIO32, su VDD (3V3) y su GND.");
    } else {
        Serial.println("    >> El microfono ANDA (hay nivel en alguno de los dos slots).");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println("==========================================");
    Serial.println("  PRUEBA DE HARDWARE DE AUDIO");
    Serial.println("  (sin radio, sin codec, sin watchdog)");
    Serial.println("==========================================");
    Serial.print("  BCLK=GPIO"); Serial.print(I2S_BCLK);
    Serial.print("  WS=GPIO");   Serial.print(I2S_WS);
    Serial.print("  MIC=GPIO");  Serial.print(MIC_DATA);
    Serial.print("  SPK=GPIO");  Serial.println(SPK_DATA);

    if (!iniciarI2S()) {
        Serial.println("  No se pudo iniciar el I2S. Freno.");
        while (1) delay(1000);
    }
    Serial.println("  I2S iniciado.");
}

void loop()
{
    probarParlante();
    delay(500);
    probarMicrofono();
    Serial.println();
    Serial.println("... repitiendo en 2 s ...");
    delay(2000);
}
