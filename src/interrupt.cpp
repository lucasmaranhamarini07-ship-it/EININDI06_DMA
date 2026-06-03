// https://docs.espressif.com/projects/arduino-esp32/en/latest/api/timer.html
#include <Arduino.h>

#define LASEC_MAX_TASKS 2
#include "util/lasecTask.h"

#include "services/lasecNet.h"
#include "services/wserial.h"
#include "services/display_ssd1306.h"
#include "services/ads1115.h"

constexpr uint8_t def_pin_SDA = 21;
constexpr uint8_t def_pin_SCL = 22;

// ---------- Taxa de amostragem ----------
// O ADS1115 em single-shot leva ~9 ms por canal a 128 SPS (padrão da lib).
// Com 2 canais: 2 × 9 ms = 18 ms de leitura bloqueante por ciclo.
// Usar SAMPLE_HZ <= 50 (período >= 20 ms) garante que a leitura termine
// antes da ISR disparar de novo.
constexpr uint32_t SAMPLE_HZ = 20; // 20 Hz → 1 leitura a cada 50 ms

// ---------- Timer de hardware ----------
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

// ---------- Callback periódico: atualiza display a cada 1 s ----------
void updateDisplayFunc() {
    disp.setText(3, ("n:" + String(_sampleCount)).c_str());
}

// ---------- Setup ----------
void setup() {
    wserial.begin();
    disp.begin(def_pin_SDA, def_pin_SCL);
    net.begin(KIT_HOSTNAME);

    disp.setText(1, (WiFi.localIP().toString() + " ID:" + String(KIT_ID)).c_str());
    disp.setText(2, KIT_HOSTNAME);
    disp.setText(3, "ISR ADS1115 20Hz");

    ads1115.begin();

    // timerBegin(freq): API do Arduino-ESP32 v3.x (espressif32 ^6.0.0).
    // Cria um timer de hardware que gera 'freq' ticks por segundo.
    _timer = timerBegin(SAMPLE_HZ);        // 20 ticks/s → 1 tick = 50 ms
    timerAttachInterrupt(_timer, &onTimer); // vincula onTimer() ao alarme
    timerAlarm(_timer, 1, true, 0);        // dispara a cada 1 tick; true = recarrega sozinho

    ltask.begin(1000);
    ltask.attach(updateDisplayFunc, 1000); // atualiza display a cada 1000 ms
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
    ltask.update();   // executa callbacks periódicos (updateDisplayFunc a cada 1 s)

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
