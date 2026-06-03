// https://docs.espressif.com/projects/arduino-esp32/en/latest/api/timer.html
#include <Arduino.h>

#include "services/lasecNet.h"
#include "services/wserial.h"
#include "services/display_ssd1306.h"
#include "services/ads1115.h"

constexpr uint8_t def_pin_SDA = 21;
constexpr uint8_t def_pin_SCL = 22;

// ---------- Timer de hardware ----------
// API v2.x: timerBegin(num, divider, countUp)
//   num=0      : timer 0 do ESP32 (0–3 disponíveis)
//   divider=80 : 80 MHz ÷ 80 = 1 MHz → 1 tick = 1 µs
//   countUp    : conta de 0 até o alarme
static hw_timer_t* _timer = nullptr;

// ---------- Flag de sincronização ISR → loop() ----------
//
// volatile: o compilador não pode cachear — ISR e loop() são contextos distintos.
// IRAM_ATTR: função fica na RAM interna (executa mesmo se a flash estiver ocupada).
// Regra de ouro: ISR faz o MÍNIMO — apenas sinaliza com a flag.
volatile bool _sampleReady = false;

void IRAM_ATTR onTimer() {
    _sampleReady = true;
}

// ---------- Dados lidos no loop() ----------
static uint16_t _pot1        = 0;
static uint16_t _pot2        = 0;
static uint32_t _sampleCount = 0;

// ---------- Funções auxiliares ----------
void readAndSend() {
    // Leitura bloqueante via I2C (~9 ms por canal a 128 SPS)
    _pot1 = ads1115.analogReadPot1(); // canal A1
    _pot2 = ads1115.analogReadPot2(); // canal A0
    _sampleCount++;

    // Envia via UDP (ou Serial se não houver link UDP ativo)
    wserial.plot("pot1", _pot1, "counts");
    wserial.plot("pot2", _pot2, "counts");
}

void updateDisplay() {
    disp.setText(1, (WiFi.localIP().toString() + " ID:" + String(KIT_ID)).c_str());
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

    // 50 ms = 50 000 µs → 20 Hz
    _timer = timerBegin(0, 80, true);
    timerAttachInterrupt(_timer, &onTimer, true); // true = borda de subida
    timerAlarmWrite(_timer, 50000, true);         // 50 000 µs; true = auto-reload
    timerAlarmEnable(_timer);
}

// ---------- Loop principal ----------
//
// Duas estratégias de temporização lado a lado:
//
//   ISR (hardware):  precisa → o timer dispara exatamente a cada 50 ms
//                    independente do que o loop() estiver fazendo.
//
//   millis():        suficiente para tarefas tolerantes a jitter,
//                    como atualizar o display ou log periódico.
//                    Vantagem: sem hardware extra, fácil de ler.
void loop() {
    wserial.update();
    disp.update();
    net.update();

    // 'now' calculado uma vez por iteração — todos os timers usam o mesmo snapshot
    const uint64_t now = millis();

    // Atualiza display a cada 1 s
    static uint64_t tDisplay = 0;
    if (now - tDisplay >= 1000) {
        tDisplay = now;
        updateDisplay();
    }

    // --- Leitura do ADS1115 controlada pela ISR do timer ---
    // _sampleReady é setado pela ISR exatamente a cada 50 ms (20 Hz)
    if (!_sampleReady) return; // CPU livre para as tarefas acima enquanto espera

    _sampleReady = false; // limpa ANTES de ler: não perde o próximo tick
    readAndSend();
}
