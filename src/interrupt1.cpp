// https://docs.espressif.com/projects/arduino-esp32/en/latest/api/timer.html
#include <Arduino.h>

#include "services/lasecNet.h"
#include "services/wserial.h"
#include "services/display_ssd1306.h"
#include "services/ads1115.h"

constexpr uint8_t def_pin_SDA = 21;
constexpr uint8_t def_pin_SCL = 22;

// ---------- Taxa de amostragem ----------
// O ADS1115 em single-shot leva ~9 ms por canal a 128 SPS (padrão da lib).
// Com 2 canais: 2 × 9 ms = 18 ms de leitura bloqueante por ciclo.
// Usar PERIOD_US >= 20000 µs garante que a leitura termine antes do próximo tick.
constexpr uint32_t PERIOD_US = 50000; // 50 ms → 20 Hz

// ---------- Timer de hardware ----------
// API do Arduino-ESP32 v2.x: timerBegin(num, divider, countUp)
//   num=0      : usa o timer 0 do ESP32 (0–3 disponíveis)
//   divider=80 : 80 MHz ÷ 80 = 1 MHz → 1 tick = 1 µs
//   countUp    : conta de 0 para cima até o alarme
static hw_timer_t* _timer = nullptr;

// ---------- Flag de sincronização ISR → loop() ----------
//
// Por que volatile?
//   A ISR e o loop() rodam em contextos distintos. Sem volatile o compilador
//   pode otimizar leituras de _sampleReady para um registrador e nunca ver
//   a atualização feita pela ISR.
//
// Regra de ouro para ISRs:
//   Faça o MÍNIMO possível — apenas sete a flag.
//   Nunca chame I2C, Serial, malloc ou qualquer função bloqueante numa ISR.
//
// IRAM_ATTR: força a função a ficar na RAM interna.
//   Sem isso, se a CPU estiver lendo a flash (ex.: durante OTA ou Wi-Fi),
//   o acesso à ISR na flash causaria uma exceção.
volatile bool _sampleReady = false;

void IRAM_ATTR onTimer() {
    _sampleReady = true; // sinaliza; o loop() faz o trabalho pesado
}

// ---------- Dados lidos no loop() ----------
static uint16_t _pot1        = 0;
static uint16_t _pot2        = 0;
static uint32_t _sampleCount = 0;

// ---------- Setup ----------
void setup() {
    wserial.begin();
    disp.begin(def_pin_SDA, def_pin_SCL);
    net.begin(KIT_HOSTNAME);

    disp.setText(1, (WiFi.localIP().toString() + " ID:" + String(KIT_ID)).c_str());
    disp.setText(2, KIT_HOSTNAME);
    disp.setText(3, "ISR ADS1115 20Hz");

    ads1115.begin();

    // timerBegin(num, divider, countUp) — API v2.x
    _timer = timerBegin(0, 80, true);
    // timerAttachInterrupt(timer, callback, edge)
    // edge=true → dispara na borda de subida do sinal de alarme
    timerAttachInterrupt(_timer, &onTimer, true);
    // timerAlarmWrite(timer, count, autoreload)
    // count=50000: 50000 µs = 50 ms → 20 Hz
    // autoreload=true: reseta e dispara de novo indefinidamente
    timerAlarmWrite(_timer, PERIOD_US, true);
    timerAlarmEnable(_timer);
}

// ---------- Loop principal ----------
//
// Padrão geral:
//   ISR (contexto de interrupção) → seta flag
//   loop()  (contexto normal)    → verifica flag → faz o trabalho
//
// Diferença em relação ao DMA (dma.cpp):
//   - DMA: hardware preenche um frame inteiro (64 amostras) sem a CPU;
//     a CPU acorda apenas 1× por frame (~3 ms a 20 kHz).
//   - Interrupção: timer acorda a CPU a cada amostra individual (a cada 50 ms).
//     Mais simples, suficiente para sensores lentos como o ADS1115.
void loop() {
    wserial.update(); // processa Serial / reativa UDP se WiFi reconectou
    disp.update();    // redesenha OLED se houve mudança de texto
    net.update();     // ArduinoOTA + mDNS

    // Atualiza display a cada 1 s sem depender de timer extra
    static uint32_t lastDisplay = 0;
    if (millis() - lastDisplay >= 1000) {
        lastDisplay = millis();
        disp.setText(3, ("n:" + String(_sampleCount)).c_str());
    }

    // Sai cedo enquanto a ISR não sinalizou — CPU livre para as tarefas acima
    if (!_sampleReady) return;

    // Limpa a flag ANTES de fazer a leitura.
    // Se limpássemos depois, uma ISR disparada durante os ~18 ms de leitura
    // seria silenciosamente ignorada.
    _sampleReady = false;

    // Leitura bloqueante via I2C (~9 ms por canal a 128 SPS)
    _pot1 = ads1115.analogReadPot1(); // canal A1
    _pot2 = ads1115.analogReadPot2(); // canal A0
    _sampleCount++;

    // Envia via UDP (ou Serial se não houver link UDP ativo)
    wserial.plot("pot1", _pot1, "counts");
    wserial.plot("pot2", _pot2, "counts");
}
