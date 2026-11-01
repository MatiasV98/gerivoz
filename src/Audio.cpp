#include "Audio.h"
#include "Config.h"

#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_0

// Cuantas muestras se convierten de una vez. Acota el tamano de los buffers
// de staging de abajo.
#define BLOQUE_MAX 128

// Staging entre el formato del DMA (int32, muestra en la parte alta) y los
// int16 con los que trabaja el resto del firmware.
static int32_t crudoIn[BLOQUE_MAX * 2];
static int32_t crudoOut[BLOQUE_MAX * 2];

bool Audio::begin()
{
    monBuf = (int16_t*)malloc(sizeof(int16_t) * MONITOR_MUESTRAS);
    if (monBuf == NULL) {
        Serial.println("Error reservando memoria del monitor");
        return false;
    }

    i2s_config_t cfg =
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),

        .sample_rate = SAMPLE_RATE,

        // 32 bits: el INMP441 entrega 24 bits alineados al MSB dentro de una
        // trama de 32. El puerto es compartido con el parlante, asi que la
        // salida usa el mismo ancho.
        // Alternativa (Opcion B) si el DAC no acepta tramas de 32 bits:
        // volver a 16 y cambiar el ancho con i2s_set_clk() antes de grabar
        // (32) y antes de reproducir (16).
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

        // ESTEREO FIJO. Antes era ONLY_LEFT (una palabra por trama), que
        // llena solo el slot izquierdo: un MAX98357A con el pin SD en
        // "derecho" no suena NADA. En estereo mandamos la muestra en los dos
        // slots y suena en cualquier posicion de SD.
        //
        // Se configura UNA VEZ y no se toca mas. Conmutar mono/estereo con
        // i2s_set_clk() al arrancar la reproduccion hace perder paquetes y
        // corta la sesion de audio en bucle (medido: de 231 recibidos a 2-20).
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,

        .communication_format = I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = I2S_DMA_BUFS,
        .dma_buf_len   = I2S_DMA_LEN,

        .use_apll = false,

        .tx_desc_auto_clear = true,

        .fixed_mclk = 0
    };

    i2s_pin_config_t pins =
    {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_WS,
        .data_out_num = SPK_DATA,
        .data_in_num  = MIC_DATA
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.print("i2s_driver_install fallo: ");
        Serial.println(esp_err_to_name(err));
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.print("i2s_set_pin fallo: ");
        Serial.println(esp_err_to_name(err));
        return false;
    }

    Serial.print("Audio inicializado | I2S ");
    Serial.print(SAMPLE_RATE);
    Serial.print(" Hz, codec ");
    Serial.print(CODEC_RATE);
    Serial.print(" Hz, DMA ");
    Serial.print(I2S_DMA_BUFS);
    Serial.print("x");
    Serial.print(I2S_DMA_LEN);
    Serial.print(" (");
    Serial.print(1000L * I2S_DMA_BUFS * I2S_DMA_LEN / SAMPLE_RATE);
    Serial.println(" ms)");

    return true;
}

void Audio::modoGrabar()
{

    // El DMA de entrada se sigue llenando mientras reproducimos, asi que al
    // arrancar a grabar hay audio viejo encolado. Si no se descarta, lo
    // primero que se transmite es lo que se escucho hace un rato.
    size_t leidos = 0;
    for (int i = 0; i < I2S_DMA_BUFS * 4; i++) {
        if (i2s_read(I2S_PORT, crudoIn, sizeof(crudoIn), &leidos, 0) != ESP_OK) break;
        if (leidos == 0) break;
    }
}

void Audio::modoReproducir()
{
    // Salida en ESTEREO a proposito, con la misma muestra en los dos canales.
    //
    // El MAX98357A elige que canal reproduce segun la tension de su pin SD
    // (derecho, izquierdo, o la semisuma). Mandando en los DOS suena sea cual
    // sea la posicion en la que haya quedado, en vez de depender de que SD
    // este exactamente donde esperamos. Medido: en mono solo se llena el slot
    // izquierdo, asi que un amplificador puesto en "derecho" no suena nada.
    i2s_zero_dma_buffer(I2S_PORT);
}

void Audio::silencio()
{
    i2s_zero_dma_buffer(I2S_PORT);
}

int Audio::leerMic(int16_t* dst, int maxN, int ticksEspera)
{
    if (maxN <= 0) return 0;
    if (maxN > BLOQUE_MAX) maxN = BLOQUE_MAX;

    // En estereo cada trama trae dos palabras. El INMP441 con L/R a GND habla
    // en un solo slot; MIC_SLOT (Config.h) dice cual.
    size_t leidos = 0;
    esp_err_t err = i2s_read(I2S_PORT, crudoIn, (size_t)maxN * 2 * sizeof(int32_t),
                             &leidos, ticksEspera);
    if (err != ESP_OK) return 0;

    int n = (int)(leidos / (2 * sizeof(int32_t)));
    for (int i = 0; i < n; i++) {
        // 24 bits alineados a MSB -> 16 bits, saturando en vez de envolver.
        // MIC_SHIFT controla la ganancia de captura (ver Config.h).
        int32_t s = crudoIn[i * 2 + MIC_SLOT] >> MIC_SHIFT;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        dst[i] = (int16_t)s;
    }
    return n;
}

int Audio::escribirParlante(const int16_t* src, int n)
{
    if (n <= 0) return 0;
    if (n > BLOQUE_MAX / 2) n = BLOQUE_MAX / 2;

    // Par L/R con la misma muestra: ver el comentario de modoReproducir().
    for (int i = 0; i < n; i++) {
        int32_t s32 = ((int32_t)src[i]) << 16;   // muestra en la parte alta
        crudoOut[i * 2]     = s32;
        crudoOut[i * 2 + 1] = s32;
    }

    size_t escritos = 0;
    esp_err_t err = i2s_write(I2S_PORT, crudoOut,
                              (size_t)n * 2 * sizeof(int32_t), &escritos, 0);
    if (err != ESP_OK) return 0;

    // El DMA se llena de a buffers enteros, multiplo del tamano de trama, asi
    // que esta division es exacta en la practica.
    return (int)(escritos / (2 * sizeof(int32_t)));
}

void Audio::cambiarFrecuencia(uint32_t fs)
{
    i2s_set_clk(I2S_PORT, fs, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(I2S_PORT);
}
