// https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_continuous.html
#include <Arduino.h>

#define LASEC_MAX_TASKS 2
#include "util/lasecTask.h"

#include "services/lasecNet.h"
#include "services/wserial.h"
#include "services/display_ssd1306.h"
#include "services/AdcDmaEsp.h"

constexpr uint8_t def_pin_SDA = 21;
constexpr uint8_t def_pin_SCL = 22;

// ---------- Parâmetros de aquisição ----------
constexpr adc_channel_t ADC_CH    = ADC_CHANNEL_3; // GPIO4 no ESP32-S3
constexpr uint32_t      SAMPLE_HZ = 20000;          // 20 kHz total

// ---------- Buffers de leitura ----------
static AdcDmaSample _samples[ADC_DMA_SAMPLES_PER_FRAME];  // amostras decodificadas
static uint16_t     _rawValues[ADC_DMA_SAMPLES_PER_FRAME]; // apenas os valores (para plot)
static uint32_t     _frameCount = 0;

// ---------- Funções auxiliares ----------

// Atualiza o display com a contagem de frames recebidos
void updateDisplayFunc() {
    disp.setText(3, ("frames: " + String(_frameCount)).c_str());
}

// ---------- Setup ----------
void setup() {
    wserial.begin();
    disp.begin(def_pin_SDA, def_pin_SCL);
    net.begin(KIT_HOSTNAME);

    disp.setText(1, (WiFi.localIP().toString() + " ID:" + String(KIT_ID)).c_str());
    disp.setText(2, KIT_HOSTNAME);
    disp.setText(3, "ADC DMA 20kHz");

    adcDma.begin(ADC_CH, SAMPLE_HZ); // configura ADC1_CH3 a 20 kHz com DMA
    adcDma.start();                   // inicia as conversões; ISR sinaliza quando frame pronto

    ltask.begin(1000);
    ltask.attach(updateDisplayFunc, 1000); // atualiza display a cada 1 s
}

// ---------- Loop principal ----------
void loop() {
    wserial.update();
    disp.update();
    net.update();
    ltask.update();

    // adcDma.available() retorna true quando a ISR sinalizou um frame completo (~3 ms a 20 kHz)
    if (!adcDma.available()) return;

    // lê e decodifica o frame; _frameCount é limpo pelo read() internamente
    size_t n = adcDma.read(_samples, ADC_DMA_SAMPLES_PER_FRAME);
    if (n == 0) return;

    _frameCount++;

    // extrai os valores brutos (0–4095) e envia o bloco via UDP/Serial
    for (size_t i = 0; i < n; i++) _rawValues[i] = _samples[i].value;
    wserial.plot("adc_raw", 0, _rawValues, n, "counts");
}
