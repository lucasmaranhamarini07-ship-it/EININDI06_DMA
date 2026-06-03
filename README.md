# II1P06 — ADC com DMA e por Interrupção no ESP32

Projeto PlatformIO com dois firmwares que demonstram estratégias diferentes de aquisição de dados analógicos:

| Firmware | Hardware | Abordagem | Arquivo |
|----------|----------|-----------|---------|
| `esp32s3-dma` | ESP32-S3 | ADC contínuo via DMA — CPU zero por amostra | `src/dma.cpp` |
| `esp32dev-irq` | ESP32 | ADS1115 via interrupção de timer — CPU acorda por amostra | `src/interrupt.cpp` |

---

## Pré-requisitos

| Item | Versão mínima |
|------|--------------|
| PlatformIO | qualquer |
| `espressif32` (platform) | `^6.0.0` (Arduino-ESP32 v3.x + ESP-IDF v5) |
| Hardware (`esp32s3-dma`) | ESP32-S3 (ex.: ESP32-S3-DevKitC-1) |
| Hardware (`esp32dev-irq`) | ESP32 clássico + ADS1115 via I2C (SDA=21, SCL=22) |

> O driver `adc_continuous` e o formato TYPE2 são exclusivos do **ESP32-S3/S2**. Para ESP32 clássico use o firmware de interrupção (`interrupt.cpp`).

---

## Configuração rápida (`platformio.ini`)

### ESP32-S3 + DMA

```ini
[env:esp32s3-dma]
platform  = espressif32 @ ^6.0.0
board     = esp32-s3-devkitc-1
framework = arduino
build_src_filter = +<dma.cpp>
build_flags =
    -DKIT_ID=0
    -DKIT_HOSTNAME='"iikit-dma"'
    -DARDUINO_USB_MODE=1        ; porta USB nativa do S3
    -DARDUINO_USB_CDC_ON_BOOT=1
```

Para gravar via USB nativa do S3 deixe as duas flags `ARDUINO_USB_*` ativas. Para gravar via OTA descomente `upload_protocol = espota` e `upload_port`.

### ESP32 + Interrupção

```ini
[env:esp32dev-irq]
platform  = espressif32 @ ^6.0.0
board     = esp32dev
framework = arduino
build_src_filter = +<interrupt.cpp>
build_flags =
    -DKIT_ID=1
    -DKIT_HOSTNAME='"iikit1"'
```

---

## Mapa de pinos — ADC1 no ESP32-S3

Somente **ADC1** pode ser usado com DMA quando o WiFi está ativo.

| Canal (`adc_channel_t`) | GPIO |
|------------------------|------|
| `ADC_CHANNEL_0` | GPIO1 |
| `ADC_CHANNEL_1` | GPIO2 |
| `ADC_CHANNEL_2` | GPIO3 |
| `ADC_CHANNEL_3` | GPIO4 |
| `ADC_CHANNEL_4` | GPIO5 |
| `ADC_CHANNEL_5` | GPIO6 |
| `ADC_CHANNEL_6` | GPIO7 |
| `ADC_CHANNEL_7` | GPIO8 |
| `ADC_CHANNEL_8` | GPIO9 |
| `ADC_CHANNEL_9` | GPIO10 |

---

## Biblioteca `AdcDmaEsp`

Arquivo: `include/services/AdcDmaEsp.h`  
Instância global disponível após o include: `adcDma`

### Estrutura de amostra

```cpp
struct AdcDmaSample {
    uint8_t  channel; // canal de origem (0–9)
    uint16_t value;   // valor 12 bits (0–4095), onde 4095 ≈ 3,3 V
};
```

### Parâmetros ajustáveis via `#define` (antes do `#include`)

| Define | Padrão | Descrição |
|--------|--------|-----------|
| `ADC_DMA_MAX_CHANNELS` | `4` | Máximo de canais simultâneos |
| `ADC_DMA_FRAME_SIZE` | `256` | Bytes por frame (múltiplo de 4); 256 → 64 amostras |
| `ADC_DMA_POOL_SIZE` | `1024` | Buffer circular DMA (bytes); maior = mais tolerância a atrasos no loop |
| `ADC_DMA_SAMPLES_PER_FRAME` | `FRAME_SIZE/4` | Amostras por frame (calculado automaticamente) |

### API

```cpp
// ── Inicialização ─────────────────────────────────────────────────────────

// 1 canal
bool begin(adc_channel_t channel, uint32_t sampleHz);

// Múltiplos canais (round-robin; frequência por canal = sampleHz / N)
bool begin(const adc_channel_t* channels, uint8_t count, uint32_t sampleHz);

// ── Controle ──────────────────────────────────────────────────────────────

bool start();   // inicia as conversões DMA
bool stop();    // pausa sem liberar recursos (pode retomar com start())
void deinit();  // libera handle e memória

// ── Leitura (padrão poll) ─────────────────────────────────────────────────

bool   available();                                     // true quando frame pronto
size_t read(AdcDmaSample* buf, size_t maxSamples);      // decodifica e retorna n amostras

// ── Leitura (padrão callback) ─────────────────────────────────────────────

void onSamplesReady(void (*cb)());  // registra callback
void update();                      // chama o callback se houver frame pronto; colocar no loop()
```

### Uso mínimo — 1 canal, padrão poll

```cpp
#include "services/AdcDmaEsp.h"

AdcDmaSample buf[ADC_DMA_SAMPLES_PER_FRAME];

void setup() {
    Serial.begin(115200);
    adcDma.begin(ADC_CHANNEL_3, 20000); // GPIO4, 20 kHz
    adcDma.start();
}

void loop() {
    if (!adcDma.available()) return;
    size_t n = adcDma.read(buf, ADC_DMA_SAMPLES_PER_FRAME);
    for (size_t i = 0; i < n; i++) {
        Serial.println(buf[i].value); // 0–4095
    }
}
```

### Uso com múltiplos canais

```cpp
const adc_channel_t chs[] = { ADC_CHANNEL_3, ADC_CHANNEL_4 };
adcDma.begin(chs, 2, 40000); // 40 kHz total → 20 kHz por canal
adcDma.start();

// No read(), buf[i].channel indica qual canal originou cada amostra.
```

### Uso com callback

```cpp
void onReady() {
    size_t n = adcDma.read(buf, ADC_DMA_SAMPLES_PER_FRAME);
    // processar amostras...
}

void setup() {
    adcDma.begin(ADC_CHANNEL_3, 20000);
    adcDma.onSamplesReady(onReady);
    adcDma.start();
}

void loop() {
    adcDma.update(); // verifica flag e chama onReady() se necessário
}
```

> Não misture `available()/read()` e `onSamplesReady()/update()` no mesmo loop.

---

## Como funciona — fluxo de dados

```
ADC1_CH3 (GPIO4)
    │
    ▼  hardware + DMA (sem CPU)
┌─────────────────────┐
│  Buffer circular DMA │  ← ADC_DMA_POOL_SIZE bytes (1024 B padrão)
│  (pool interno)      │
└──────────┬──────────┘
           │ a cada ADC_DMA_FRAME_SIZE bytes (256 B = 64 amostras)
           ▼
    ISR _isrConvDone()          ← roda em IRAM, contexto de interrupção
    seta _ready = true
           │
           ▼
    loop() → adcDma.available() == true
    → adcDma.read() drena o frame
    → decodifica formato TYPE2 → AdcDmaSample[]
    → wserial.plot() envia array via UDP (ou Serial)
```

### Formato TYPE2 (32 bits por amostra, ESP32-S3)

```
 31      17  16   13 12  11               0
 ┌─────────┬──────┬────┬──────────────────┐
 │ reserv. │ unit │ ch │      data        │
 └─────────┴──────┴────┴──────────────────┘
  unit: 0=ADC1, 1=ADC2
  ch:   canal de origem (4 bits)
  data: valor 12 bits (0–4095)
```

O `read()` faz `reinterpret_cast` para `adc_digi_output_data_t` e extrai os campos `type2.channel` e `type2.data`.

---

## Como o firmware `dma.cpp` funciona

Arquivo: `src/dma.cpp`

### Setup

```
wserial.begin()          → Serial 115200 + socket UDP na porta 47268
disp.begin(SDA=21,SCL=22) → display OLED SSD1306
net.begin("iikit-dma")   → WiFi (WiFiManager) + mDNS + OTA
adcDma.begin(CH3, 20kHz) → configura ADC1_CH3 (GPIO4) a 20 kHz com DMA
adcDma.start()           → inicia as conversões; ISR começa a sinalizar
ltask.begin(1000)        → timer de hardware a 1 kHz (base 1 ms)
ltask.attach(cb, 1000)   → atualiza display a cada 1000 ms
```

### Loop principal

```cpp
void loop() {
    wserial.update(); // processa Serial / reativa UDP se WiFi reconectou
    disp.update();    // redesenha OLED se houve mudança de texto
    net.update();     // ArduinoOTA + mDNS
    ltask.update();   // executa callbacks periódicos (updateDisplayFunc a cada 1 s)

    if (!adcDma.available()) return;  // aguarda ISR sinalizar frame pronto (~3 ms a 20 kHz)

    size_t n = adcDma.read(_samples, ADC_DMA_SAMPLES_PER_FRAME); // decodifica
    if (n == 0) return;

    _frameCount++;

    // extrai só os valores e envia o array inteiro de uma vez via UDP
    for (size_t i = 0; i < n; i++) _rawValues[i] = _samples[i].value;
    wserial.plot("adc_raw", 0, _rawValues, n, "counts");
}
```

A função `wserial.plot(nome, dt_ms, array, len, unidade)` serializa o array no formato de plotter serial com timestamps incrementais e envia via UDP (ou Serial se não houver link UDP ativo).

### Cadência de frames

| Parâmetro | Valor |
|-----------|-------|
| Taxa de amostragem | 20 000 Hz |
| Amostras por frame | 64 (`ADC_DMA_FRAME_SIZE=256`, 4 bytes/amostra) |
| Intervalo entre frames | 64/20000 = **3,2 ms** |
| Loop livre p/ outras tarefas | ~3,2 ms por frame |

---

## Como o firmware `interrupt.cpp` funciona

Arquivo: `src/interrupt.cpp` — env: `esp32dev-irq`

### Setup

```
wserial.begin()           → Serial 115200 + socket UDP na porta 47268
disp.begin(SDA=21,SCL=22) → display OLED SSD1306
net.begin("iikit1")       → WiFi (WiFiManager) + mDNS + OTA
ads1115.begin()           → ADC externo ADS1115 via I2C
timerBegin(20)            → timer de hardware a 20 Hz (1 tick = 50 ms)
timerAlarm(..., 1, true)  → alarme a cada tick; dispara onTimer() via ISR
ltask.attach(cb, 1000)    → atualiza display a cada 1000 ms
```

### Padrão ISR → flag → loop()

```
Timer HW (20 Hz)
    │
    ▼  ISR: onTimer()            ← IRAM_ATTR, contexto de interrupção
    seta _sampleReady = true     ← única coisa feita aqui
    │
    ▼  loop()                    ← contexto normal
    if (!_sampleReady) return    ← CPU livre para wserial/disp/net/ltask
    _sampleReady = false         ← limpa ANTES de ler (não perde próximo tick)
    ads1115.read() × 2 canais    ← ~18 ms bloqueante no I2C
    wserial.plot() × 2           ← envia via UDP (ou Serial)
```

### Por que `volatile`?

A ISR e o `loop()` rodam em contextos distintos (interrupção vs. thread principal). Sem `volatile`, o compilador pode otimizar `_sampleReady` para um registrador e o `loop()` nunca vê a atualização feita pela ISR. `volatile` força releitura da RAM a cada acesso.

### Por que `IRAM_ATTR`?

A flash do ESP32 pode ficar temporariamente indisponível enquanto o Wi-Fi ou o OTA usam o barramento de flash. Se a ISR residir na flash nesse momento, a CPU tenta executar código inacessível → exceção (guru meditation). `IRAM_ATTR` move a função para a RAM interna, garantindo execução sempre disponível.

### Por que limpar a flag **antes** de ler?

```
_sampleReady = false;   // ← aqui
_pot1 = ads1115.read(); // ~9 ms bloqueante
_pot2 = ads1115.read(); // ~9 ms bloqueante
```

A leitura do ADS1115 leva ~18 ms. Se o timer dispara durante esse tempo (lembra: 50 ms de período, 18 ms de leitura — há margem, mas a lógica deve ser correta para qualquer configuração), a ISR seta `_sampleReady = true` novamente. Se limpássemos **depois**, esse segundo sinal seria sobrescrito para `false` e a amostra seria perdida.

### Cadência de amostras

| Parâmetro | Valor |
|-----------|-------|
| Timer | 20 Hz |
| Período entre amostras | 50 ms |
| Tempo de leitura (2 canais × 9 ms) | ~18 ms |
| CPU livre por ciclo | ~32 ms |
| Taxa efetiva por canal | 20 Hz |

---

## DMA vs. Interrupção — quando usar cada abordagem

| | Interrupção (`interrupt.cpp`) | DMA (`dma.cpp`) |
|-|-------------------------------|-----------------|
| **Hardware** | ESP32 clássico | ESP32-S3 |
| **Sensor** | ADS1115 (externo, I2C) | ADC interno |
| **Taxa típica** | até ~50 Hz | até 83 kHz |
| **CPU por amostra** | acordada 1× | zero |
| **CPU por frame (64 am.)** | 64× | 1× |
| **Complexidade** | simples | maior (driver ESP-IDF) |
| **Adequado para** | sensores lentos, I2C/SPI | sinais rápidos, áudio, vibração |

> **Regra prática:** se a taxa desejada cabe no `loop()` sem stress, use interrupção. Se a taxa exige que a CPU fique fora do caminho crítico, use DMA.

---

## Serviços auxiliares utilizados

| Instância global | Header | Usado em | Função |
|-----------------|--------|----------|--------|
| `adcDma` | `services/AdcDmaEsp.h` | `dma.cpp` | ADC contínuo com DMA (ESP32-S3) |
| `ads1115` | `services/ads1115.h` | `interrupt.cpp` | ADC externo ADS1115 via I2C |
| `wserial` | `services/wserial.h` | ambos | Serial/UDP + plotter |
| `disp` | `services/display_ssd1306.h` | ambos | OLED SSD1306 via I2C |
| `net` | `services/lasecNet.h` | ambos | WiFi (WiFiManager) + mDNS + OTA |
| `ltask` | `util/lasecTask.h` | ambos | Escalonador cooperativo por timer |

---

## Referências

- [ESP-IDF: ADC Continuous Mode](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_continuous.html)
- [Arduino-ESP32 v3.x ADC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html)
- [Arduino-ESP32 v3.x Timer API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/timer.html)
