#pragma once
/**
 * @file lasecTask.h
 * @brief Escalonador cooperativo baseado em timer de hardware.
 *
 * O timer dispara a `frequency` Hz. Cada tarefa tem um limite de ticks;
 * ao atingi-lo, o callback é enfileirado em lasecQueue e executado em loop().
 *
 * Uso:
 *   #define LASEC_MAX_TASKS 3   // opcional — padrão: 4
 *   #include "util/lasecTask.h"
 *
 *   jtask.begin(1000);               // timer a 1 kHz → base de 1 ms
 *   jtask.attach(minhaTarefa,  50);  // executa a cada  50 ms
 *   jtask.attach(outraTarefa, 500);  // executa a cada 500 ms
 *
 *   void loop() { jtask.update(); }
 */

#include <Arduino.h>
#include "util/lasecQueue.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <esp_idf_version.h>
// Duas APIs de timer coexistem no ecossistema ESP32 Arduino:
//   API nova (ESP-IDF v5 / Arduino-ESP32 v3.x): timerBegin(freq), timerAlarm()
//   API velha (ESP-IDF v4 / Arduino-ESP32 v2.x): timerBegin(num,div,up), timerAlarmWrite()
// Usamos ESP_IDF_VERSION_MAJOR para selecionar a correta em tempo de compilação.

#elif defined(ARDUINO_ARCH_AVR)
#include <avr/interrupt.h>
#include <avr/io.h>
#endif

#ifndef ARDUINO_ISR_ATTR
#define ARDUINO_ISR_ATTR
#endif

#ifndef LASEC_MAX_TASKS
#define LASEC_MAX_TASKS 4
#endif

typedef void (*lasecTask_callback_t)();

/**
 * @brief Escalonador cooperativo baseado em timer de hardware.
 * @tparam MaxTasks Número máximo de tarefas registráveis (padrão: LASEC_MAX_TASKS).
 */
template <uint8_t MaxTasks = LASEC_MAX_TASKS>
class lasecTask {
public:
    /**
     * @brief Inicializa o timer de hardware e começa a disparar ticks.
     * @param frequency Frequência do timer em Hz (ex.: 1000 → base de 1 ms).
     * @return true se o timer foi iniciado com sucesso.
     */
    bool begin(uint32_t frequency) {
        if (frequency == 0) return false;
        if (!lasecQueueCreate(&_queue)) return false;
        for (uint8_t i = 0; i < _taskCount; ++i) _tasks[i].counter = 0;
        _inst = this;

#if defined(ARDUINO_ARCH_ESP32)
        if (_timer != nullptr) { timerEnd(_timer); _timer = nullptr; }
#  if ESP_IDF_VERSION_MAJOR >= 5
        // API nova — ESP-IDF v5 / Arduino-ESP32 v3.x
        _timer = timerBegin(frequency);
        if (_timer == nullptr) return false;
        timerAttachInterrupt(_timer, &lasecTask::_isr);
        timerAlarm(_timer, 1, true, 0);
#  else
        // API velha — ESP-IDF v4 / Arduino-ESP32 v2.x (pioarduino)
        // divider=80: 80 MHz ÷ 80 = 1 MHz → 1 tick = 1 µs
        // alarme em 1.000.000/frequency µs = 1 período
        _timer = timerBegin(0, 80, true);
        if (_timer == nullptr) return false;
        timerAttachInterrupt(_timer, &lasecTask::_isr, true);
        timerAlarmWrite(_timer, 1000000UL / frequency, true);
        timerAlarmEnable(_timer);
#  endif
        return true;
#elif defined(ARDUINO_ARCH_AVR)
        return _setupAvrTimer2(frequency);
#else
        return false;
#endif
    }

    /**
     * @brief Registra uma função para ser chamada periodicamente.
     * @param task       Callback void() a executar.
     * @param limitTicks Número de ticks entre cada execução.
     * @return true se a tarefa foi adicionada.
     */
    bool attach(lasecTask_callback_t task, uint16_t limitTicks) {
        if (task == nullptr || limitTicks == 0) return false;

#if defined(ARDUINO_ARCH_ESP32)
        portENTER_CRITICAL(&_mux);
#elif defined(ARDUINO_ARCH_AVR)
        const uint8_t sreg = SREG; cli();
#else
        noInterrupts();
#endif

        bool ok = false;
        if (_taskCount < MaxTasks) {
            // Atribuição explícita: compatível com C++11 (structs com default-member-init
            // não são agregados em C++11, então {x,y,z} não funciona como assignment)
            _tasks[_taskCount].counter = 0;
            _tasks[_taskCount].limit   = limitTicks;
            _tasks[_taskCount].cb      = task;
            ++_taskCount;
            ok = true;
        }

#if defined(ARDUINO_ARCH_ESP32)
        portEXIT_CRITICAL(&_mux);
#elif defined(ARDUINO_ARCH_AVR)
        SREG = sreg;
#else
        interrupts();
#endif
        return ok;
    }

    /**
     * @brief Drena a fila e executa os callbacks pendentes.
     *        Chamar no loop() principal.
     */
    void update() {
        lasecTask_callback_t fn = nullptr;
        while (lasecQueueReceive(&_queue, &fn)) {
            if (fn) fn();
        }
    }

    // Público apenas para acesso pelo vetor de ISR do AVR
    static void ARDUINO_ISR_ATTR _isr() {
        if (_inst) _inst->_tick();
    }

private:
    struct _Task {
        uint16_t              counter = 0;
        uint16_t              limit   = 0;
        lasecTask_callback_t  cb      = nullptr;
    };

    _Task        _tasks[MaxTasks] = {};
    uint8_t      _taskCount       = 0;
    lasecQueue_t _queue           = {};

#if defined(ARDUINO_ARCH_ESP32)
    hw_timer_t  *_timer = nullptr;
    portMUX_TYPE _mux   = portMUX_INITIALIZER_UNLOCKED;
#endif

    static lasecTask *_inst;

    void ARDUINO_ISR_ATTR _tick() {
#if defined(ARDUINO_ARCH_ESP32)
        portENTER_CRITICAL_ISR(&_mux);
#endif
        for (uint8_t i = 0; i < _taskCount; ++i) {
            if (++_tasks[i].counter >= _tasks[i].limit) {
                if (lasecQueueSendFromISR(&_queue, _tasks[i].cb)) {
                    _tasks[i].counter = 0;
                } else {
                    _tasks[i].counter = _tasks[i].limit - 1;
                }
            }
        }
#if defined(ARDUINO_ARCH_ESP32)
        portEXIT_CRITICAL_ISR(&_mux);
#endif
    }

#if defined(ARDUINO_ARCH_AVR)
    bool _setupAvrTimer2(uint32_t frequency) {
        struct PS { uint16_t divisor; uint8_t bits; };
        static const PS kPS[] = {
            {1,   0b00000001}, {8,    0b00000010}, {32,   0b00000011},
            {64,  0b00000100}, {128,  0b00000101}, {256,  0b00000110},
            {1024,0b00000111},
        };
        for (const PS &p : kPS) {
            const uint32_t denom = (uint32_t)p.divisor * frequency;
            if (!denom) continue;
            const uint32_t cnt = (F_CPU + denom / 2UL) / denom;
            if (!cnt || cnt > 256UL) continue;
            const uint8_t sreg = SREG; cli();
            TCCR2A = _BV(WGM21);
            TCCR2B = p.bits;
            TCNT2  = 0;
            OCR2A  = (uint8_t)(cnt - 1UL);
            TIFR2  = _BV(OCF2A);
            TIMSK2 = _BV(OCIE2A);
            SREG = sreg;
            return true;
        }
        return false;
    }
#endif
};

// Definição do membro estático (obrigatória para templates em headers)
template <uint8_t N>
lasecTask<N> *lasecTask<N>::_inst = nullptr;

#if defined(ARDUINO_ARCH_AVR)
// Vetor de ISR do AVR — roteia para a instância global padrão
ISR(TIMER2_COMPA_vect) { lasecTask<LASEC_MAX_TASKS>::_isr(); }
#endif

/// Instância global padrão
inline lasecTask<LASEC_MAX_TASKS> ltask;
