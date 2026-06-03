#include "Arduino.h"
#include <EEPROM.h>
#include "services/lasecNet.h"
#include "services/wserial.h"
#include "services/ads1115.h"
#include "services/display_ssd1306.h"
#include "util/lasecDebounce.h"

constexpr uint8_t def_pin_D1 = 23;
constexpr uint8_t def_pin_D2 = 19;
constexpr uint8_t def_pin_SCL = 22;     ///< GPIO para SCL do display OLED.
constexpr uint8_t def_pin_SDA = 21;     ///< GPIO para SDA do display OLED.

lasecDebounce push1,push2,rtn1,rtn2;

void blinkLEDFunc(uint8_t pin) {
    digitalWrite(pin, !digitalRead(pin));
}

void managerInputFunc(void) {
    const uint16_t vlPOT1 = ads1115.analogReadPot1();
    const uint16_t vlPOT2 = ads1115.analogReadPot2();
    disp.setText(2, ("P1:" + String(vlPOT1) + "  P2:" + String(vlPOT2)).c_str());
    wserial.plot("vlPOT1", vlPOT1);
    wserial.plot("vlPOT2", vlPOT2);
}

void setup()
{
  wserial.begin();
  disp.begin(def_pin_SDA, def_pin_SCL);
  ads1115.begin();
  net.begin(KIT_HOSTNAME);
  
  disp.setText(1, (WiFi.localIP().toString() + " ID:" + String(KIT_ID)).c_str());
  disp.setText(2, KIT_HOSTNAME);
  disp.setText(3, "");

  pinMode(def_pin_D1, OUTPUT);
  pinMode(def_pin_D2, OUTPUT);  
  
  push1.begin(18, INPUT_PULLUP, LOW, 20);
  push2.begin(19, INPUT_PULLUP, LOW, 20);
  rtn1.begin(18, INPUT_PULLUP, LOW, 20);
  rtn2.begin(18, INPUT_PULLUP, LOW, 20);
  
  delay(50);  
}

void loop()
{
  wserial.update();
  disp.update();
  net.update();
  
  const uint64_t now = millis();

  static uint64_t t1 = 0;
  if ((now - t1) >= 500)
  {
    t1 = now;
    blinkLEDFunc(def_pin_D1);
  }

  static uint64_t t2 = 0;
  if ((now - t2) >= 1000)
  {
    t2 = now;
    blinkLEDFunc(def_pin_D2);
  }

  static uint64_t t3 = 0;  
  if ((now - t3) >= 50)
  {
    t3 = now;
    managerInputFunc();
  } 
}