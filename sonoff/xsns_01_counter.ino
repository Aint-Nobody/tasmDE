/*
  xsns_01_counter.ino - Counter sensors (water meters, electricity meters etc.) sensor support for Sonoff-Tasmota

  Copyright (C) 2019  Maarten Damen and Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*********************************************************************************************\
 * Counter sensors (water meters, electricity meters etc.)
\*********************************************************************************************/

#define XSNS_01             1
#define GPIO_LEVEL (99)

static inline bool is_gpio_cnt_low(int index){
   return ((pin[GPIO_CNTR1 + index] < GPIO_LEVEL)?true:false);
}

unsigned long last_counter_timer[MAX_COUNTERS]; // Last counter time in micro seconds

void CounterUpdate(uint8_t index)
{
  unsigned long counter_debounce_time = micros() - last_counter_timer[index -1];
  if (counter_debounce_time > Settings.pulse_counter_debounce * 1000) {
    last_counter_timer[index -1] = micros();
    if (bitRead(Settings.pulse_counter_type, index -1)) {
      RtcSettings.pulse_counter[index -1] = counter_debounce_time;
    } else {
      RtcSettings.pulse_counter[index -1]++;
    }

//    snprintf_P(log_data, sizeof(log_data), PSTR("CNTR: Interrupt %d"), index);
//    AddLog(LOG_LEVEL_DEBUG);
  }
}

void CounterUpdate1(void)
{
  CounterUpdate(1);
}

void CounterUpdate2(void)
{
  CounterUpdate(2);
}

void CounterUpdate3(void)
{
  CounterUpdate(3);
}

void CounterUpdate4(void)
{
  CounterUpdate(4);
}

/********************************************************************************************/

void CounterSaveState(void)
{
  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    if (is_gpio_cnt_low(i)) {
      Settings.pulse_counter[i] = RtcSettings.pulse_counter[i];
    }
  }
}

void CounterInit(void)
{
  typedef void (*function) () ;
  function counter_callbacks[] = { CounterUpdate1, CounterUpdate2, CounterUpdate3, CounterUpdate4 };

  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    if (is_gpio_cnt_low(i)) {
      pinMode(pin[GPIO_CNTR1 +i], bitRead(counter_no_pullup, i) ? INPUT : INPUT_PULLUP);
      attachInterrupt(pin[GPIO_CNTR1 +i], counter_callbacks[i], FALLING);
    }
  }
}

#ifdef USE_WEBSERVER
const char HTTP_SNS_COUNTER[] PROGMEM =
  "%s{s}" D_COUNTER "%d{m}%s%s{e}";  // {s} = <tr><th>, {m} = </th><td>, {e} = </td></tr>
#endif  // USE_WEBSERVER

void CounterShow(bool json)
{
  char stemp[10];

  uint8_t dsxflg = 0;
  uint8_t header = 0;
  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    uint32_t cnt = RtcSettings.pulse_counter[i];
    if (is_gpio_cnt_low(i)) {
      char counter[33];
      if (bitRead(Settings.pulse_counter_type, i)) {
        dtostrfd((double)cnt / 1000000, 6, counter);
      } else {
        dsxflg++;
        snprintf_P(counter, sizeof(counter), PSTR("%lu"), (unsigned long)cnt);
      }

      if (json) {
        if (!header) {
          snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s,\"COUNTER\":{"), mqtt_data);
          stemp[0] = '\0';
        }
        header++;
        snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"C%d\":%s"), mqtt_data, stemp, i +1, counter);
        strlcpy(stemp, ",", sizeof(stemp));
#ifdef USE_DOMOTICZ
        if ((0 == tele_period) && (1 == dsxflg)) {
          DomoticzSensor(DZ_COUNT, cnt);
          dsxflg++;
        }
#endif  // USE_DOMOTICZ
#ifdef USE_WEBSERVER
      } else {
        snprintf_P(mqtt_data, sizeof(mqtt_data), HTTP_SNS_COUNTER, mqtt_data, i +1, counter, (bitRead(Settings.pulse_counter_type, i)) ? " " D_UNIT_SECOND : "");
#endif  // USE_WEBSERVER
      }
    }
    if (bitRead(Settings.pulse_counter_type, i)) {
      RtcSettings.pulse_counter[i] = 0xFFFFFFFF;  // Set Timer to max in case of no more interrupts due to stall of measured device
    }
  }
  if (json) {
    if (header) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s}"), mqtt_data);
    }
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xsns01(uint8_t function)
{
  bool result = false;

  switch (function) {
    case FUNC_INIT:
      CounterInit();
      break;
    case FUNC_JSON_APPEND:
      CounterShow(1);
      break;
#ifdef USE_WEBSERVER
    case FUNC_WEB_APPEND:
      CounterShow(0);
      break;
#endif  // USE_WEBSERVER
    case FUNC_SAVE_BEFORE_RESTART:
      CounterSaveState();
      break;
  }
  return result;
}
