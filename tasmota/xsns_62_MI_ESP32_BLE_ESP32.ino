/*
  xsns_62_MI_ESP32.ino - MI-BLE-sensors via ESP32 support for Tasmota

  Copyright (C) 2020  Christian Baars and Theo Arends

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


  --------------------------------------------------------------------------------------------
  Version yyyymmdd  Action    Description
  --------------------------------------------------------------------------------------------
  0.9.1.7 20201116  changed - small bugfixes, add BLOCK and OPTION command, send BLE scan via MQTT
  -------
  0.9.1.6 20201022  changed - Beacon support, RSSI at TELEPERIOD, refactoring
  -------
  0.9.1.5 20201021  changed - HASS related ('null', hold back discovery), number of found sensors for RULES
  -------
  0.9.1.4 20201020  changed - use BearSSL for decryption, revert to old TELEPERIOD-cycle as default
  -------
  0.9.1.3 20200926  changed - Improve HA discovery, make key+MAC case insensitive
  -------
  0.9.1.3 20200916  changed - add ATC (custom FW for LYWSD03MMC), API adaption for NimBLE-Arduino 1.0.2
  -------
  0.9.1.2 20200802  changed - add MHO-C303
  -------
  0.9.1.1 20200715  changed - add MHO-C401, refactoring
  -------
  0.9.1.0 20200712  changed - add lights and yeerc, add pure passive mode with decryption,
                              lots of refactoring
  -------
  0.9.0.1 20200706  changed - adapt to new NimBLE-API, tweak scan process
  -------
  0.9.0.0 20200413  started - initial development by Christian Baars
                    forked  - from arendst/tasmota            - https://github.com/arendst/Tasmota

*/
#define VSCODE_DEV

#ifdef VSCODE_DEV
#define ESP32
#define USE_BLE_ESP32
#define USE_MI_ESP32
#endif

// for testing of BLE_ESP32, we remove xsns_62_MI_ESP32.ino completely, and instead add this modified xsns_52_ibeacon_BLE_ESP32.ino
#ifdef USE_BLE_ESP32

#ifdef ESP32                       // ESP32 only. Use define USE_HM10 for ESP8266 support

#ifdef USE_MI_ESP32

#define XSNS_62                    62
#define USE_MI_DECRYPTION

#include "xdrv_47_BLE_ESP32.h"
#include <vector>
#ifdef USE_MI_DECRYPTION
#include <t_bearssl.h>
#endif //USE_MI_DECRYPTION

void MI32scanEndedCB(NimBLEScanResults results);
void MI32notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify);

struct {
  uint16_t perPage = 4;
  uint32_t period;             // set manually in addition to TELE-period, is set to TELE-period after start
  int secondsCounter = 0;   // counts up in MI32EverySecond to period
  union {
    struct {
      uint32_t init:1;
      uint32_t shallClearResults:1;   // BLE scan results
      uint32_t shallShowStatusInfo:1; // react to amount of found sensors via RULES
      uint32_t firstAutodiscoveryDone:1;
      uint32_t activeBeacon:1;
      uint32_t shallShowScanResult:1;
      uint32_t shallShowBlockList:1;
      uint32_t shallTriggerTele:1;
      uint32_t triggeredTele:1;
    };
    uint32_t all = 0;
  } mode;

  struct {
    // the slot currently having it's battery read
    // set to 0 to start a battery read cycle
    uint8_t slot = 255; 
    uint8_t active = 0; 
  } batteryreader;

  struct {
    // the slot currently having it's battery read
    // set to 0 to start a battery read cycle
    uint8_t slot = 255; 
    uint8_t active = 0; 
  } sensorreader;

  struct {
    uint8_t sensor;             // points to to the number 0...255
    uint8_t beaconScanCounter;  // countdown timer in seconds
  } state;

  struct {
    uint32_t allwaysAggregate:1; // always show all known values of one sensor in brdigemode
    uint32_t noSummary:1;        // no sensor values at TELE-period
    uint32_t directBridgeMode:1; // send every received BLE-packet as a MQTT-message in real-time
    uint32_t holdBackFirstAutodiscovery:1; // allows to trigger it later
    uint32_t showRSSI:1;
    uint32_t ignoreBogusBattery:1;
    uint32_t minimalSummary:1;   // DEPRECATED!!
  } option;
} MI32;

#pragma pack(1)  // byte-aligned structures to read the sensor data

  struct {
    int16_t temp;
    uint8_t hum;
    uint16_t volt; // LYWSD03 only
  } LYWSD0x_HT;
  struct {
    uint8_t spare;
    int16_t temp;
    uint16_t hum;
  } CGD1_HT;
  struct {
    int16_t temp;
    uint8_t spare;
    uint32_t lux;
    uint8_t moist;
    uint16_t fert;
  } Flora_TLMF; // temperature, lux, moisture, fertility


struct mi_beacon_t{
  uint16_t frame;
  uint16_t productID;
  uint8_t counter;
  uint8_t MAC[6];
  uint8_t spare;
  uint8_t type;
  uint8_t ten;
  uint8_t size;
  union {
    struct{ //0d
      int16_t temp;
      uint16_t hum;
    }HT;
    uint8_t bat; //0a
    int16_t temp; //04
    uint16_t hum; //06
    uint32_t lux; //07
    uint8_t moist; //08
    uint16_t fert; //09
    uint32_t NMT; //17
    struct{ //01
      uint16_t num;
      uint8_t longPress;
    }Btn;
  };
  uint8_t padding[12];
};

struct cg_packet_t {
  uint16_t frameID;
  uint8_t MAC[6];
  uint16_t mode;
  union {
    struct {
    int16_t temp;  // -9 - 59 °C
    uint16_t hum;
    };
    uint8_t bat;
  };
};

struct encPacket_t{
  // the packet is longer, but this part is enough to decrypt
  uint16_t PID;
  uint8_t frameCnt;
  uint8_t MAC[6];
  uint8_t payload[16]; // only a pointer to the address, size is variable
};

union mi_bindKey_t{
  struct{
    uint8_t key[16];
    uint8_t MAC[6];
    };
  uint8_t buf[22];
};

struct ATCPacket_t{
  uint8_t MAC[6];
  uint16_t temp; //sadly this is in wrong endianess
  uint8_t hum;
  uint8_t batPer;
  uint16_t batMV;
  uint8_t frameCnt;
};

#pragma pack(0)

struct mi_sensor_t{
  uint8_t type; //Flora = 1; MI-HT_V1=2; LYWSD02=3; LYWSD03=4; CGG1=5; CGD1=6
  uint8_t lastCnt; //device generated counter of the packet
  uint8_t shallSendMQTT;
  uint8_t MAC[6];
  union {
    struct {
      uint32_t temp:1;
      uint32_t hum:1;
      uint32_t tempHum:1; //every hum sensor has temp too, easier to use Tasmota dew point functions
      uint32_t lux:1;
      uint32_t moist:1;
      uint32_t fert:1;
      uint32_t bat:1;
      uint32_t NMT:1;
      uint32_t PIR:1;
      uint32_t Btn:1;
    };
    uint32_t raw;
  } feature;
  union {
    struct {
      uint32_t temp:1;
      uint32_t hum:1;
      uint32_t tempHum:1; //can be combined from the sensor
      uint32_t lux:1;
      uint32_t moist:1;
      uint32_t fert:1;
      uint32_t bat:1;
      uint32_t NMT:1;
      uint32_t motion:1;
      uint32_t noMotion:1;
      uint32_t Btn:1;
    };
    uint32_t raw;
  } eventType;

  int RSSI;
  uint32_t lastTime;
  uint32_t lux;
  float temp; //Flora, MJ_HT_V1, LYWSD0x, CGx
  union {
    struct {
      uint8_t moisture;
      uint16_t fertility;
      char firmware[6]; // actually only for FLORA but hopefully we can add for more devices
    }; // Flora
    struct {
      float hum;
    }; // MJ_HT_V1, LYWSD0x
    struct {
      uint16_t events; //"alarms" since boot
      uint32_t NMT;    // no motion time in seconds for the MJYD2S
    };
    uint16_t Btn;
  };
  union {
      uint8_t bat; // many values seem to be hard-coded garbage (LYWSD0x, GCD1)
  };
};

struct scan_entry_t {
  uint8_t MAC[6];
  uint16_t CID;
  uint16_t SVC;
  uint16_t UUID;
  int32_t RSSI;
};

struct generic_beacon_t {
  uint8_t MAC[6];
  uint32_t time;
  int32_t RSSI;
  uint16_t CID; // company identifier
  uint16_t UUID; // the first, if more than one exists
  uint16_t SVC;
  bool active = false;
};

struct MAC_t {
  uint8_t buf[6];
};

std::vector<mi_sensor_t> MIBLEsensors;
std::vector<mi_bindKey_t> MIBLEbindKeys;
std::array<generic_beacon_t,4> MIBLEbeacons; // we support a fixed number
std::vector<scan_entry_t> MIBLEscanResult;
std::vector<MAC_t> MIBLEBlockList;

static BLEScan* MI32Scan;

/*********************************************************************************************\
 * constants
\*********************************************************************************************/

#define D_CMND_MI32 "MI32"

const char kMI32_Commands[] PROGMEM = D_CMND_MI32 "|"
#ifdef USE_MI_DECRYPTION
  "Key|"
#endif  // USE_MI_DECRYPTION
  "Period|Time|Page|Battery|Unit|Beacon|Block|Option";

void (*const MI32_Commands[])(void) PROGMEM = {
#ifdef USE_MI_DECRYPTION
  &CmndMi32Key,
#endif  // USE_MI_DECRYPTION
  &CmndMi32Period, &CmndMi32Time, &CmndMi32Page, &CmndMi32Battery, &CmndMi32Unit, &CmndMi32Beacon, &CmndMi32Block, &CmndMi32Option };

#define FLORA       1
#define MJ_HT_V1    2
#define LYWSD02     3
#define LYWSD03MMC  4
#define CGG1        5
#define CGD1        6
#define NLIGHT      7
#define MJYD2S      8
#define YEERC       9
#define MHOC401     10
#define MHOC303     11
#define ATC         12

#define MI32_TYPES    12 //count this manually

const uint16_t kMI32DeviceID[MI32_TYPES]={ 0x0098, // Flora
                                  0x01aa, // MJ_HT_V1
                                  0x045b, // LYWSD02
                                  0x055b, // LYWSD03
                                  0x0347, // CGG1
                                  0x0576, // CGD1
                                  0x03dd, // NLIGHT
                                  0x07f6, // MJYD2S
                                  0x0153, // yee-rc
                                  0x0387, // MHO-C401
                                  0x06d3, // MHO-C303
                                  0x0a1c  // ATC -> this is a fake ID
                                  };

const char kMI32DeviceType1[] PROGMEM = "Flora";
const char kMI32DeviceType2[] PROGMEM = "MJ_HT_V1";
const char kMI32DeviceType3[] PROGMEM = "LYWSD02";
const char kMI32DeviceType4[] PROGMEM = "LYWSD03";
const char kMI32DeviceType5[] PROGMEM = "CGG1";
const char kMI32DeviceType6[] PROGMEM = "CGD1";
const char kMI32DeviceType7[] PROGMEM = "NLIGHT";
const char kMI32DeviceType8[] PROGMEM = "MJYD2S";
const char kMI32DeviceType9[] PROGMEM = "YEERC";
const char kMI32DeviceType10[] PROGMEM ="MHOC401";
const char kMI32DeviceType11[] PROGMEM ="MHOC303";
const char kMI32DeviceType12[] PROGMEM ="ATC";
const char * kMI32DeviceType[] PROGMEM = {kMI32DeviceType1,kMI32DeviceType2,kMI32DeviceType3,kMI32DeviceType4,kMI32DeviceType5,kMI32DeviceType6,kMI32DeviceType7,kMI32DeviceType8,kMI32DeviceType9,kMI32DeviceType10,kMI32DeviceType11,kMI32DeviceType12};

typedef int BATREAD_FUNCTION(int slot);
typedef int UNITWRITE_FUNCTION(int slot, int unit);
typedef int TIMEWRITE_FUNCTION(int slot);

int genericOpCompleteFn(BLE_ESP32::generic_sensor_t *pStruct);
int genericBatReadFn(int slot);
int genericUnitWriteFn(int slot, int unit);
int genericTimeWriteFn(int slot);
int MI32scanCompleteCallback(NimBLEScanResults results);

const char LYWSD02_Svc[] PROGMEM = "ebe0ccb0-7a0a-4b0c-8a1a6ff2997da3a6";
const char LYWSD02_BattChar[] PROGMEM = "EBE0CCC4-7A0A-4B0C-8A1A6FF2997DA3A6";
const char LYWSD02_UnitChar[] PROGMEM = "EBE0CCBE-7A0A-4B0C-8A1A6FF2997DA3A6";
const char LYWSD02_TimeChar[] PROGMEM = "EBE0CCB7-7A0A-4B0C-8A1A6FF2997DA3A6";
const char LYWSD02_BattNotifyChar[] PROGMEM = "ebe0ccc1-a0a-4b0c-8a1a6ff2997da3a6";


const char *LYWSD03_Svc = LYWSD02_Svc;
const char *LYWSD03_BattNotifyChar = LYWSD02_BattNotifyChar;

const char *MHOC303_Svc = LYWSD02_Svc;
const char *MHOC303_UnitChar = LYWSD02_UnitChar;
const char *MHOC303_TimeChar = LYWSD02_TimeChar;

const char *MHOC401_Svc = LYWSD02_Svc;
const char *MHOC401_BattNotifyChar = LYWSD02_BattNotifyChar;

const char CGD1_Svc[] PROGMEM = "180F"; 
const char CGD1_BattChar[] PROGMEM = "2A19"; 

const char FLORA_Svc[] PROGMEM = "00001204-0000-1000-800000805f9b34fb";
const char FLORA_BattChar[] PROGMEM = "00001a02-0000-1000-800000805f9b34fb";





/*********************************************************************************************\
 * enumerations
\*********************************************************************************************/

enum MI32_Commands {          // commands useable in console or rules
  CMND_MI32_PERIOD,           // set period like TELE-period in seconds between read-cycles
  CMND_MI32_TIME,             // set LYWSD02-Time from ESP8266-time
  CMND_MI32_PAGE,             // sensor entries per web page, which will be shown alternated
  CMND_MI32_BATTERY,          // read all battery levels
  CMND_MI32_UNIT,             // toggles the displayed unit between C/F (LYWSD02)
  CMND_MI32_KEY,              // add bind key to a mac for packet decryption
  CMND_MI32_BEACON,           // add up to 4 beacons defined by their MAC addresses
  CMND_MI32_BLOCK,            // block BLE sensors defined by their MAC addresses
  CMND_MI32_OPTION            // change driver options at runtime
  };

enum MI32_TASK {
       MI32_TASK_SCAN = 0,
       MI32_TASK_CONN = 1,
       MI32_TASK_TIME = 2,
       MI32_TASK_BATT = 3,
       MI32_TASK_UNIT = 4,
};

enum MI32_BEACON_CMND {
       MI32_BEACON_ON  = 0,
       MI32_BEACON_OFF = 1,
       MI32_BEACON_DEL = 2,
};


// types of operaiton performed, included in context
enum MI32_MI_OP_TYPES {
  OP_TIME_WRITE = 0,
  OP_BATT_READ = 1,
  OP_UNIT_WRITE = 2,
  OP_UNIT_READ = 3,
  OP_UNIT_TOGGLE = 4,
  OP_READ_HT_LY = 5,
};


/*********************************************************************************************\
 * Classes
\*********************************************************************************************/


// fn type READ_CALLBACK
// NOTE!!!: this callback is called DIRECTLY from the operation task, so be careful about cross-thread access of data
// if is called after read, so that you can do a read/modify/write operation on a characteristic.
int toggleUnit(BLE_ESP32::generic_sensor_t *op){
  uint32_t context = (uint32_t) op->context;
  int opType = context >> 24;
  // we only need to op type
  int devType = (context >> 16) & 0xff;
  int slot = (context) & 0xff;
  switch (opType){
    case OP_UNIT_TOGGLE:{
      uint8_t curUnit = 0;
      if( op->dataRead[0] != 0 && op->dataRead[0] < 101 ){
          curUnit = op->dataRead[0];
      }

      curUnit = curUnit == 0x01?0xFF:0x01;  // C/F
      // copy in ALL of the data, because we don't know how long this is from the existing src code.
      memcpy(op->dataToWrite, op->dataRead, op->readlen);
      op->writelen = op->readlen;
      op->dataToWrite[0] = curUnit;
    } break;
    case OP_UNIT_WRITE:{
      uint8_t curUnit = op->dataToWrite[0];
      // copy in ALL of the data, because we don't know how long this is from the existing src code.
      memcpy(op->dataToWrite, op->dataRead, op->readlen);
      op->writelen = op->readlen;
      op->dataToWrite[0] = curUnit;
    } break;
  }
  return 0;
}

bool MI32Operation(int slot, int optype, const char *svc, const char *charactistic, const char *notifychar = nullptr, const uint8_t *data = nullptr, int datalen = 0) {

  BLE_ESP32::generic_sensor_t *op = new BLE_ESP32::generic_sensor_t;
  memset(op, 0, sizeof(BLE_ESP32::generic_sensor_t));
  BLE_ESP32::dump(op->MAC, sizeof(op->MAC), MIBLEsensors[slot].MAC, 6) ;

  if (!svc || !svc[0]){
    return 0;
  }

  bool havechar = false;
  
  strncpy(op->serviceStr, svc, sizeof(op->serviceStr));
  if (charactistic && charactistic[0]){
    havechar = true;
    strncpy(op->characteristicStr, charactistic, sizeof(op->characteristicStr));
  }

  if (notifychar && notifychar[0]){
    strncpy(op->notificationCharacteristicStr, notifychar, sizeof(op->notificationCharacteristicStr));
  }    

  if (data && datalen) {
    op->writelen = datalen;
    memcpy(op->dataToWrite, data, datalen);
  } else {
    if (!datalen && havechar){
      op->readlen = 1; // if we don't set readlen, then it won't read
    }
  }


  // the only times we intercept between read abnd write
  if ((optype == OP_UNIT_WRITE) || (optype == OP_UNIT_TOGGLE)){
    op->readlen = 1; // if we don't set readlen, then it won't read
    op->readmodifywritecallback = (void *)toggleUnit;
  }

  // this op will call us back on complete or failure.
  op->completecallback = (void *)genericOpCompleteFn;
  uint32_t context = (optype << 24) | (MIBLEsensors[slot].type << 16) | slot;
  op->context = (void *)context;

  return BLE_ESP32::extQueueOperation(&op);
}



int genericBatReadFn(int slot){
  int res = 0;
  switch(MIBLEsensors[slot].type) {
    // these use notify for battery read
    case LYWSD03MMC:
      res = MI32Operation(slot, OP_BATT_READ, LYWSD03_Svc, nullptr, LYWSD03_BattNotifyChar);
      break;
    case MHOC401:
      res = MI32Operation(slot, OP_BATT_READ, MHOC401_Svc, nullptr, MHOC401_BattNotifyChar);
      break;

    // these read a characteristic
    case FLORA: 
      res = MI32Operation(slot, OP_BATT_READ, FLORA_Svc, FLORA_BattChar);
      break;
    case LYWSD02: 
      res = MI32Operation(slot, OP_BATT_READ, LYWSD02_Svc, LYWSD02_BattChar);
      break;
    case CGD1:
      res = MI32Operation(slot, OP_BATT_READ, CGD1_Svc, CGD1_BattChar);
      break;

    default:
      res = -1;
      break;
  }
  return res;
}



int genericSensorReadFn(int slot){
  int res = 0;
  switch(MIBLEsensors[slot].type) {
    case LYWSD02:
      res = MI32Operation(slot, OP_READ_HT_LY, LYWSD02_Svc, nullptr, LYWSD02_BattNotifyChar);
      break;
    case LYWSD03MMC:
      res = MI32Operation(slot, OP_READ_HT_LY, LYWSD03_Svc, nullptr, LYWSD03_BattNotifyChar);
      break;
    case MHOC401:
      res = MI32Operation(slot, OP_READ_HT_LY, MHOC401_Svc, nullptr, MHOC401_BattNotifyChar);
      break;

    default:
      res = -1;
      break;
  }
  return res;
}


// called once per second
int readOneSensor(){
  if (MI32.sensorreader.active){
    return 0;
  }
  if (MI32.sensorreader.slot >= MIBLEsensors.size()){
    return 0;
  }

  int res = genericSensorReadFn(MI32.sensorreader.slot);

  // if this sensor in this slot does not need to be read via notify, just move on top the next one
  if (res < 0){
    MI32.sensorreader.slot++;
    return 0;
  }

  if (res == 0){
    // can't read at the moment (no operations available?)
    return 0;
  }

  // setup next slot to read
  MI32.sensorreader.slot++;
  // and make it wait until the read/notify is complete
  // this is cleared in the response callback.
  MI32.sensorreader.active = 1;

  // started one
  return 1;
}



// called once per second
int readOneBat(){
  if (MI32.batteryreader.active){
    return 0;
  }
  if (MI32.batteryreader.slot >= MIBLEsensors.size()){
    return 0;
  }

  int res = genericBatReadFn(MI32.batteryreader.slot);

  // if this sensor in this slot does not support battery read, just move on top the next one
  if (res < 0){
    MI32.batteryreader.slot++;
    return 0;
  }

  if (res == 0){
    // can't read at the moment (no operations available?)
    return 0;
  }

  // setup next slot to read
  MI32.batteryreader.slot++;
  // and make it wait until the read/notify is complete
  // this is cleared in the response callback.
  MI32.batteryreader.active = 1;

  // started one
  return 1;
}



/////////////////////////////////////////////////////
// change the unit of measurement?
// call with unit == -1 to cause the unit to be toggled.
int genericUnitWriteFn(int slot, int unit){
  int res = 0;
  int op = OP_UNIT_WRITE;
  if (unit == -1){
    op = OP_UNIT_TOGGLE;
  }
  uint8_t writeData[1];
  writeData[0] = unit;
  switch (MIBLEsensors[slot].type){
    case LYWSD02:
      res = MI32Operation(slot, op, LYWSD02_Svc, LYWSD02_UnitChar, nullptr, writeData, 1);
      break;
    case MHOC303: // actually, EXACTLY the same as above, including the sevice and characteristic...
      res = MI32Operation(slot, op, MHOC303_Svc, MHOC303_UnitChar, nullptr, writeData, 1);
      break;
    default:
      res = -1;
      break;
  }
  return res;
}

/////////////////////////////////////////////////////
// read the unit of measurement.  genericOpCompleteFn
int genericUnitReadFn(int slot){
  int res = 0;
  switch (MIBLEsensors[slot].type){
    case LYWSD02:
      res = MI32Operation(slot, OP_UNIT_READ, LYWSD02_Svc, LYWSD02_UnitChar);
      break;
    case MHOC303: // actually, EXACTLY the same as above, including the sevice and characteristic...
      res = MI32Operation(slot, OP_UNIT_READ, MHOC303_Svc, MHOC303_UnitChar);
      break;
    default:
      res = -1;
      break;
  }
  return res;
}


/////////////////////////////////////////////////////
// write time to a device. genericOpCompleteFn
int genericTimeWriteFn(int slot){
  int res = 0;
  switch (MIBLEsensors[slot].type){
    case LYWSD02: {
      union {
        uint8_t buf[5];
        uint32_t time;
      } _utc;
      _utc.time = Rtc.utc_time;
      _utc.buf[4] = Rtc.time_timezone / 60;
      res = MI32Operation(slot, OP_TIME_WRITE, LYWSD02_Svc, LYWSD02_TimeChar, nullptr, _utc.buf, sizeof(_utc.buf));
    } break;
    case MHOC303: // actually, EXACTLY the same as above, including the sevice and characteristic...
      union {
        uint8_t buf[5];
        uint32_t time;
      } _utc;
      _utc.time = Rtc.utc_time;
      _utc.buf[4] = Rtc.time_timezone / 60;
      res = MI32Operation(slot, OP_TIME_WRITE, MHOC303_Svc, MHOC303_TimeChar, nullptr, _utc.buf, sizeof(_utc.buf));
      break;
    default:
      res = -1;
      break;
  }
  return res;
}


int genericOpCompleteFn(BLE_ESP32::generic_sensor_t *op){
  uint32_t context = (uint32_t) op->context;
  int opType = context >> 24;
  int devType = (context >> 16) & 0xff;
  int slot = (context) & 0xff;

  char slotMAC[13];
  BLE_ESP32::dump(slotMAC, sizeof(slotMAC), MIBLEsensors[slot].MAC, 6) ;

  if (strncmp(slotMAC, op->MAC, 12)){
    // slot changed during operation?
    AddLog_P(LOG_LEVEL_ERROR,PSTR("Slot mac changed during an operation"));
    return 0;
  }

  if (op->state & GEN_STATE_FAILED){
    AddLog_P(LOG_LEVEL_ERROR,PSTR("operation failed %x"), op->state);
    return 0;
  }

  switch(opType){
    case OP_TIME_WRITE:
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("Time write for %s complete"), slotMAC);
      return 0; // nothing to do
    case OP_BATT_READ:{
      uint8_t *data = nullptr;
      int len = 0;
      if (op->notifylen){
        data = op->dataNotify;
        len = op->notifylen;
      }
      if (op->readlen){
        data = op->dataRead;
        len = op->readlen;
      }
      MIParseBatt(slot, data, len);

      // allow another...
      MI32.batteryreader.active = 0;
    } return 0;

    case OP_UNIT_WRITE: // nothing more to do?
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("Unit write for %s complete"), slotMAC);
      return 0;

    case OP_UNIT_READ: {
      uint8_t currUnit = op->dataRead[0];  
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("Unit read for %s complete %d"), slotMAC, currUnit);
    } return 0;

    case OP_UNIT_TOGGLE: {
      uint8_t currUnit = op->dataToWrite[0];  
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("Unit toggle for %s complete %d->%d; datasize was %d"), slotMAC, op->dataRead[0], op->dataToWrite[0], op->readlen);
    } return 0;

    case OP_READ_HT_LY: {
      MI32notifyHT_LY(slot, (char*)op->dataNotify, op->notifylen);
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("HT_LY notify for %s complete"), slotMAC);
    } return 0;

    default:
      AddLog_P(LOG_LEVEL_ERROR,PSTR("OpType %d not recognised?"), opType);
      return 0;
  }

  return 0;
}


int MI32advertismentCallback(BLE_ESP32::ble_advertisment_t *pStruct)
{
  // we will try not to use this...
  BLEAdvertisedDevice *advertisedDevice = pStruct->advertisedDevice;

  // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Advertised Device: %s Buffer: %u"),advertisedDevice->getAddress().toString().c_str(),advertisedDevice->getServiceData(0).length());
  int RSSI = pStruct->RSSI;
  const uint8_t *addr = pStruct->addr;

  if (pStruct->svcdataCount == 0) {
    MI32HandleGenericBeacon(pStruct->payload, pStruct->payloadLen, RSSI, addr);
    return 0;
  }

  if (pStruct->svcdata[0].serviceUUID->u.type == 16){
    uint16_t UUID = pStruct->svcdata[0].serviceUUID->u16.value;
    size_t ServiceDataLength = pStruct->svcdata[0].serviceDataLen;
    char * ServiceData = (char *)pStruct->svcdata[0].serviceData;
    if(UUID==0xfe95) {
      if(MI32isInBlockList(addr) == true) return 0;
      MI32ParseResponse(ServiceData, ServiceDataLength, addr, RSSI);
    }
    else if(UUID==0xfdcd) {
      if(MI32isInBlockList(addr) == true) return 0;
      MI32parseCGD1Packet(ServiceData, ServiceDataLength, addr, RSSI);
    }
    else if(UUID==0x181a) { //ATC
      if(MI32isInBlockList(addr) == true) return 0;
      MI32ParseATCPacket(ServiceData, ServiceDataLength, addr, RSSI);
    }
    else {
      if(MI32.state.beaconScanCounter!=0 || MI32.mode.activeBeacon){
        MI32HandleGenericBeacon(pStruct->payload, pStruct->payloadLen, RSSI, addr);
      }
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("No Xiaomi Device: %x: %s Buffer: %u"), UUID, advertisedDevice->getAddress().toString().c_str(),advertisedDevice->getServiceData(0).length());
      // MI32Scan->erase(advertisedDevice->getAddress());
    }
  }
  return 0;
}


/*********************************************************************************************\
 * Helper functions
\*********************************************************************************************/

/**
 * @brief Remove all colons from null terminated char array
 *
 * @param _string Typically representing a MAC-address like AA:BB:CC:DD:EE:FF
 */
void MI32stripColon(char* _string){
  uint32_t _length = strlen(_string);
  uint32_t _index = 0;
  while (_index < _length) {
    char c = _string[_index];
    if(c==':'){
      memmove(_string+_index,_string+_index+1,_length-_index);
    }
    _index++;
  }
  _string[_index] = 0;
}

/**
 * @brief Convert string that repesents a hexadecimal number to a byte array
 *
 * @param _string input string in format: AABBCCDDEEFF or AA:BB:CC:DD:EE:FF, caseinsensitive
 * @param _mac  target byte array must match the correct size (i.e. AA:BB -> uint8_t bytes[2])
 */

void MI32HexStringToBytes(char* _string, uint8_t* _byteArray) {
  MI32stripColon(_string);
  UpperCase(_string,_string);
  uint32_t index = 0;
  uint32_t _end = strlen(_string);
  memset(_byteArray,0,_end/2);
  while (index < _end) {
      char c = _string[index];
      uint8_t value = 0;
      if(c >= '0' && c <= '9')
        value = (c - '0');
      else if (c >= 'A' && c <= 'F')
        value = (10 + (c - 'A'));
      _byteArray[(index/2)] += value << (((index + 1) % 2) * 4);
      index++;
  }
}

/**
 * @brief Reverse an array of 6 bytes
 *
 * @param _mac a byte array of size 6 (typicalliy representing a MAC address)
 */
void MI32_ReverseMAC(uint8_t _mac[]){
  uint8_t _reversedMAC[6];
  for (uint8_t i=0; i<6; i++){
    _reversedMAC[5-i] = _mac[i];
  }
  memcpy(_mac,_reversedMAC, sizeof(_reversedMAC));
}

#ifdef USE_MI_DECRYPTION
void MI32AddKey(char* payload){
  mi_bindKey_t keyMAC;
  MI32HexStringToBytes(payload,keyMAC.buf);
  bool unknownKey = true;
  for(uint32_t i=0; i<MIBLEbindKeys.size(); i++){
    if(memcmp(keyMAC.MAC,MIBLEbindKeys[i].MAC,sizeof(keyMAC.MAC))==0){
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("known key"));
      unknownKey=false;
    }
  }
  if(unknownKey){
    AddLog_P(LOG_LEVEL_DEBUG,PSTR("New key"));
    MIBLEbindKeys.push_back(keyMAC);
  }
}

/**
 * @brief Decrypts payload in place
 *
 * @param _buf - pointer to the buffer at position of PID
 * @param _bufSize - buffersize (last position is two bytes behind last byte of TAG)
 * @param _type - sensor type
 * @return int - error code, 0 for success
 */
int MI32_decryptPacket(char *_buf, uint16_t _bufSize, uint32_t _type){
  encPacket_t *packet = (encPacket_t*)_buf;
  uint8_t payload[8];
  size_t data_len = _bufSize - 9 - 4 - 3 - 1 - 1 ; // _bufsize - header - tag - ext.counter - RSSI - spare(?)
  int ret = 0;
  uint8_t nonce[12];
  uint32_t tag;
  const unsigned char authData[1] = {0x11};

  // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Decrypt Size of Buffer: %u, payload length: %u"), _bufSize, data_len);
  // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t*)_buf, _bufSize);

  // nonce: device MAC, device type, frame cnt, ext. cnt
  for (uint32_t i = 0; i<6; i++){
    nonce[i] = packet->MAC[i];
  }
  memcpy((uint8_t*)&nonce+6,(uint8_t*)&packet->PID,2);
  nonce[8] = packet->frameCnt;
  memcpy((uint8_t*)&nonce+9,(uint8_t*)&_buf[_bufSize-9],3);
  // AddLog_P(LOG_LEVEL_DEBUG,PSTR("nonceCnt1 and 2: %02x %02x %02x"),nonce[9],nonce[10],nonce[11]);
  memcpy((uint8_t*)&tag,(uint8_t*)&_buf[_bufSize-6],4);
  // AddLog_P(LOG_LEVEL_DEBUG,PSTR("tag: %02x %02x %02x %02x"),tag[0],tag[1],tag[2],tag[3]);

  MI32_ReverseMAC(packet->MAC);
  uint8_t _bindkey[16] = {0x0};
  bool foundNoKey = true;
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("MI32: search key for MAC: %02x  %02x  %02x  %02x  %02x  %02x"), packet->MAC[0], packet->MAC[1], packet->MAC[2], packet->MAC[3], packet->MAC[4], packet->MAC[5]);
  for(uint32_t i=0; i<MIBLEbindKeys.size(); i++){
    if(memcmp(packet->MAC,MIBLEbindKeys[i].MAC,sizeof(packet->MAC))==0){
      memcpy(_bindkey,MIBLEbindKeys[i].key,sizeof(_bindkey));
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("MI32: decryption Key found"));
      foundNoKey = false;
    break;
    }
  }
  if(foundNoKey){
    AddLog_P(LOG_LEVEL_DEBUG,PSTR("MI32: no Key found !!"));
    return -2;
  }

  br_aes_small_ctrcbc_keys keyCtx;
  br_aes_small_ctrcbc_init(&keyCtx, _bindkey, sizeof(_bindkey));

  br_ccm_context ctx;
  br_ccm_init(&ctx, &keyCtx.vtable);
  br_ccm_reset(&ctx, nonce, sizeof(nonce), sizeof(authData), data_len, sizeof(tag));
  br_ccm_aad_inject(&ctx, authData, sizeof(authData));
  br_ccm_flip(&ctx);


  memcpy(payload,packet->payload,data_len); //we want to be sure about 4-byte alignement
  br_ccm_run(&ctx, 0, payload, data_len);
  memcpy((uint8_t*)packet->payload+1,payload,data_len); //back to the packet

  ret = br_ccm_check_tag(&ctx, &tag);

  AddLog_P(LOG_LEVEL_DEBUG,PSTR("MI32: Err:%i, Decrypted : %02x  %02x  %02x  %02x  %02x "), ret, packet->payload[1],packet->payload[2],packet->payload[3],packet->payload[4],packet->payload[5]);
  return ret-1;
}
#endif // USE_MI_DECRYPTION

#ifdef USE_HOME_ASSISTANT
/**
 * @brief For HASS only, changes last entry of JSON in mqtt_data to 'null'
 */

void MI32nullifyEndOfMQTT_DATA(){
  char *p = TasmotaGlobal.mqtt_data + strlen(TasmotaGlobal.mqtt_data);
  while(true){
    *p--;
    if(p[0]==':'){
      p[1] = 0;
      break;
    }
  }
  ResponseAppend_P(PSTR("null"));
}
#endif // USE_HOME_ASSISTANT

/*********************************************************************************************\
 * common functions
\*********************************************************************************************/


/**
 * @brief Return the slot number of a known sensor or return create new sensor slot
 *
 * @param _MAC     BLE address of the sensor
 * @param _type       Type number of the sensor
 * @return uint32_t   Known or new slot in the sensors-vector
 */
uint32_t MIBLEgetSensorSlot(uint8_t (&_MAC)[6], uint16_t _type, uint8_t counter){

  DEBUG_SENSOR_LOG(PSTR("%s: will test ID-type: %x"),D_CMND_MI32, _type);
  bool _success = false;
  for (uint32_t i=0;i<MI32_TYPES;i++){ // i < sizeof(kMI32DeviceID) gives compiler warning
    if(_type == kMI32DeviceID[i]){
      DEBUG_SENSOR_LOG(PSTR("MI32: ID is type %u"), i);
      _type = i+1;
      _success = true;
    }
    else {
      DEBUG_SENSOR_LOG(PSTR("%s: ID-type is not: %x"),D_CMND_MI32,kMI32DeviceID[i]);
    }
  }
  if(!_success) return 0xff;

  DEBUG_SENSOR_LOG(PSTR("%s: vector size %u"),D_CMND_MI32, MIBLEsensors.size());
  for(uint32_t i=0; i<MIBLEsensors.size(); i++){
    if(memcmp(_MAC,MIBLEsensors[i].MAC,sizeof(_MAC))==0){
      DEBUG_SENSOR_LOG(PSTR("%s: known sensor at slot: %u"),D_CMND_MI32, i);
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Counters: %x %x"),MIBLEsensors[i].lastCnt, counter);
      if(MIBLEsensors[i].lastCnt==counter) {
        // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Old packet"));
        return 0xff; // packet received before, stop here
      }
      return i;
    }
    DEBUG_SENSOR_LOG(PSTR("%s: i: %x %x %x %x %x %x"),D_CMND_MI32, MIBLEsensors[i].MAC[5], MIBLEsensors[i].MAC[4],MIBLEsensors[i].MAC[3],MIBLEsensors[i].MAC[2],MIBLEsensors[i].MAC[1],MIBLEsensors[i].MAC[0]);
    DEBUG_SENSOR_LOG(PSTR("%s: n: %x %x %x %x %x %x"),D_CMND_MI32, _MAC[5], _MAC[4], _MAC[3],_MAC[2],_MAC[1],_MAC[0]);
  }
  DEBUG_SENSOR_LOG(PSTR("%s: found new sensor"),D_CMND_MI32);
  mi_sensor_t _newSensor;
  memcpy(_newSensor.MAC,_MAC, sizeof(_MAC));
  _newSensor.type = _type;
  _newSensor.eventType.raw = 0;
  _newSensor.feature.raw = 0;
  _newSensor.temp =NAN;
  _newSensor.bat=0x00;
  _newSensor.RSSI=0xffff;
  _newSensor.lux = 0x00ffffff;
  switch (_type)
    {
    case FLORA:
      _newSensor.moisture =0xff;
      _newSensor.fertility =0xffff;
      _newSensor.firmware[0]='\0';
      _newSensor.feature.temp=1;
      _newSensor.feature.moist=1;
      _newSensor.feature.fert=1;
      _newSensor.feature.lux=1;
      _newSensor.feature.bat=1;
      break;
    case NLIGHT:
      _newSensor.events=0x00;
      _newSensor.feature.PIR=1;
      _newSensor.feature.NMT=1;
      break;
    case MJYD2S:
      _newSensor.NMT=0;
      _newSensor.events=0x00;
      _newSensor.feature.PIR=1;
      _newSensor.feature.NMT=1;
      _newSensor.feature.lux=1;
      _newSensor.feature.bat=1;
      break;
    case YEERC:
      _newSensor.feature.Btn=1;
      break;
    default:
      _newSensor.hum=NAN;
      _newSensor.feature.temp=1;
      _newSensor.feature.hum=1;
      _newSensor.feature.tempHum=1;
      _newSensor.feature.bat=1;
      break;
    }
  MIBLEsensors.push_back(_newSensor);
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("%s: new %s at slot: %u"),D_CMND_MI32, kMI32DeviceType[_type-1],MIBLEsensors.size()-1);
  MI32.mode.shallShowStatusInfo = 1;
  return MIBLEsensors.size()-1;
};

/**
 * @brief trigger real-time message for PIR or RC
 *
 */
void MI32triggerTele(void){
    MI32.mode.triggeredTele = 1;
    ResponseClear();
    if (MqttShowSensor()) {
      MqttPublishPrefixTopic_P(TELE, PSTR(D_RSLT_SENSOR), Settings.flag.mqtt_sensor_retain);
  #ifdef USE_RULES
      RulesTeleperiod();  // Allow rule based HA messages
  #endif  // USE_RULES
    }
}

/**
 * @brief Is called after every finding of new BLE sensor
 *
 */
void MI32StatusInfo() {
  MI32.mode.shallShowStatusInfo = 0;
  Response_P(PSTR("{\"%s\":{\"found\":%u}}"), D_CMND_MI32, MIBLEsensors.size());
  XdrvRulesProcess();
}

/*********************************************************************************************\
 * BLE callbacks section
 * These are called from main thread only.
\*********************************************************************************************/


int MI32scanCompleteCallback(NimBLEScanResults results){
  AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: scancomplete"));
  return 0;
}


/*********************************************************************************************\
 * init BLE_32
\*********************************************************************************************/


void MI32Init(void) {
  MIBLEsensors.reserve(10);
  MIBLEbindKeys.reserve(10);
  MIBLEscanResult.reserve(20);
  MI32.mode.init = false;

  //test section for options
  MI32.option.allwaysAggregate = 1;
  MI32.option.noSummary = 0;
  MI32.option.minimalSummary = 0;
  MI32.option.directBridgeMode = 0;
  MI32.option.showRSSI = 1;
  MI32.option.ignoreBogusBattery = 1; // from advertisements
  MI32.option.holdBackFirstAutodiscovery = 1;

  BLE_ESP32::registerForAdvertismentCallbacks((const char *)"iBeacon", MI32advertismentCallback);
  BLE_ESP32::registerForScanCallbacks((const char *)"iBeacon", MI32scanCompleteCallback);
  // note: for operations, we will set individual callbacks in the operations we request
  //void registerForOpCallbacks(const char *tag, BLE_ESP32::OPCOMPLETE_CALLBACK* pFn);

  AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: init: request callbacks"));
  MI32.period = Settings.tele_period;
  return;
}


/*********************************************************************************************\
 * Task section
\*********************************************************************************************/




int MIParseBatt(int slot, uint8_t *data, int len){
  int value = data[0];
  char slotMAC[13];
  BLE_ESP32::dump(slotMAC, sizeof(slotMAC), MIBLEsensors[slot].MAC, 6) ;

  if ((value != 0) && (value < 101)){
    MIBLEsensors[slot].bat = value;
    if(MIBLEsensors[slot].type==FLORA){
      if (len < 7){
        AddLog_P(LOG_LEVEL_ERROR,PSTR("FLORA: not enough bytes read for firmware?"));
      } else {
        memcpy(MIBLEsensors[slot].firmware, data+2, 5);
        MIBLEsensors[slot].firmware[5] = '\0';
        AddLog_P(LOG_LEVEL_DEBUG,PSTR("%s: FLORA Firmware: %s"),D_CMND_MI32,MIBLEsensors[slot].firmware);
      }
    }
    MIBLEsensors[slot].eventType.bat  = 1;
    MIBLEsensors[slot].shallSendMQTT = 1;
    MI32.mode.shallTriggerTele = 1;
    AddLog_P(LOG_LEVEL_DEBUG,PSTR("Batt read for %s complete %d"), slotMAC, value);
  } else {
    AddLog_P(LOG_LEVEL_ERROR,PSTR("Batt read for %s complete but out of range 1-101 (%d)"), slotMAC, value);
  }

  return 0;
}

/*********************************************************************************************\
 * parse the response from advertisements
\*********************************************************************************************/

void MI32parseMiBeacon(char * _buf, uint32_t _slot, uint16_t _bufSize){
  float _tempFloat;
  mi_beacon_t _beacon;

  if (MIBLEsensors[_slot].type==MJ_HT_V1 || MIBLEsensors[_slot].type==CGG1 || MIBLEsensors[_slot].type==YEERC){
    memcpy((uint8_t*)&_beacon+1,(uint8_t*)_buf, sizeof(_beacon)-1); // shift by one byte for the MJ_HT_V1 DANGER!!!
    memcpy((uint8_t*)&_beacon.MAC,(uint8_t*)&_beacon.MAC+1,6);      // but shift back the MAC
    _beacon.counter = _buf[4];                                      // restore the counter
  }
  else{
    memcpy((char *)&_beacon, _buf, _bufSize);
  }

  MIBLEsensors[_slot].lastCnt = _beacon.counter;
#ifdef USE_MI_DECRYPTION
  int decryptRet = 0;
  switch(MIBLEsensors[_slot].type){
    case LYWSD03MMC: case MHOC401:
      if (_beacon.frame == 0x5858){
        decryptRet = MI32_decryptPacket((char*)&_beacon.productID,_bufSize, LYWSD03MMC); //start with PID
        // AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t*)&_beacon.productID,_bufSize);
      }
      else return; // 0x3058 holds no data, TODO: check for unpaired devices, that need connections
      break;
    case MJYD2S:
      AddLog_P(LOG_LEVEL_DEBUG,PSTR("MJYD2S: %x"),_beacon.frame);
      if (_beacon.frame == 0x5948){                                               // Now let's build/recreate a special MiBeacon
        memmove((uint8_t*)&_beacon.MAC+6,(uint8_t*)&_beacon.MAC, _bufSize);       // shift payload by the size of the MAC = 6 bytes
        memcpy((uint8_t*)&_beacon.MAC,MIBLEsensors[_slot].MAC,6);                 // now insert the real MAC from our internal vector
        _bufSize+=6;                                                              // the packet has grown
        MI32_ReverseMAC(_beacon.MAC);                                             // payload MAC is always reversed
        AddLog_P(LOG_LEVEL_DEBUG,PSTR("MJYD2S: special packet"));
      }
      if (_beacon.frame != 0x5910){
        decryptRet = MI32_decryptPacket((char*)&_beacon.productID,_bufSize,MJYD2S); //start with PID
      }
      break;
  }
if(decryptRet!=0){
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("MI32: decryption failed with error: %d"),decryptRet);
  return;
}
#endif //USE_MI_DECRYPTION

  if(MIBLEsensors[_slot].type==6){
    DEBUG_SENSOR_LOG(PSTR("CGD1 no support for MiBeacon, type %u"),MIBLEsensors[_slot].type);
    return;
  }
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("%s at slot %u with payload type: %02x"), kMI32DeviceType[MIBLEsensors[_slot].type-1],_slot,_beacon.type);
  switch(_beacon.type){
    case 0x01:
      MIBLEsensors[_slot].Btn=_beacon.Btn.num + (_beacon.Btn.longPress/2)*6;
      MIBLEsensors[_slot].eventType.Btn = 1;
      MI32.mode.shallTriggerTele = 1;
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 1: U16:  %u Button"), MIBLEsensors[_slot].Btn );
    break;
    case 0x04:
      _tempFloat=(float)(_beacon.temp)/10.0f;
      if(_tempFloat<60){
        MIBLEsensors[_slot].temp=_tempFloat;
        MIBLEsensors[_slot].eventType.temp = 1;
        DEBUG_SENSOR_LOG(PSTR("Mode 4: temp updated"));
      }
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 4: U16:  %u Temp"), _beacon.temp );
    break;
    case 0x06:
      _tempFloat=(float)(_beacon.hum)/10.0f;
      if(_tempFloat<101){
        MIBLEsensors[_slot].hum=_tempFloat;
        MIBLEsensors[_slot].eventType.hum = 1;
        DEBUG_SENSOR_LOG(PSTR("Mode 6: hum updated"));
      }
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 6: U16:  %u Hum"), _beacon.hum);
    break;
    case 0x07:
      MIBLEsensors[_slot].lux=_beacon.lux & 0x00ffffff;
      if(MIBLEsensors[_slot].type==MJYD2S){
        MIBLEsensors[_slot].eventType.noMotion  = 1;
      }
      MIBLEsensors[_slot].eventType.lux  = 1;
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 7: U24: %u Lux"), _beacon.lux & 0x00ffffff);
    break;
    case 0x08:
      MIBLEsensors[_slot].moisture=_beacon.moist;
      MIBLEsensors[_slot].eventType.moist  = 1;
      DEBUG_SENSOR_LOG(PSTR("Mode 8: moisture updated"));
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 8: U8: %u Moisture"), _beacon.moist);
    break;
    case 0x09:
           MIBLEsensors[_slot].fertility=_beacon.fert;
           MIBLEsensors[_slot].eventType.fert  = 1;
          DEBUG_SENSOR_LOG(PSTR("Mode 9: fertility updated"));
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 9: U16: %u Fertility"), _beacon.fert);
    break;
    case 0x0a:
      if(MI32.option.ignoreBogusBattery){
        if(MIBLEsensors[_slot].type==LYWSD03MMC || MIBLEsensors[_slot].type==MHOC401){
          break;
        }
      }
      if(_beacon.bat<101){
        MIBLEsensors[_slot].bat = _beacon.bat;
        MIBLEsensors[_slot].eventType.bat  = 1;
        DEBUG_SENSOR_LOG(PSTR("Mode a: bat updated"));
        }
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode a: U8: %u %%"), _beacon.bat);
    break;
    case 0x0d:
      _tempFloat=(float)(_beacon.HT.temp)/10.0f;
      if(_tempFloat<60){
          MIBLEsensors[_slot].temp = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode d: temp updated"));
      }
      _tempFloat=(float)(_beacon.HT.hum)/10.0f;
      if(_tempFloat<100){
          MIBLEsensors[_slot].hum = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode d: hum updated"));
      }
      MIBLEsensors[_slot].eventType.tempHum  = 1;
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode d: U16:  %x Temp U16: %x Hum"), _beacon.HT.temp,  _beacon.HT.hum);
    break;
#ifdef USE_MI_DECRYPTION
    case 0x0f:
    if (_beacon.ten!=0) break;
      MIBLEsensors[_slot].eventType.motion = 1;
      MIBLEsensors[_slot].lastTime = millis();
      MIBLEsensors[_slot].events++;
      MIBLEsensors[_slot].lux = _beacon.lux;
      MIBLEsensors[_slot].eventType.lux = 1;
      MIBLEsensors[_slot].NMT = 0;
      MI32.mode.shallTriggerTele = 1;
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("PIR: primary"),MIBLEsensors[_slot].lux );
    break;
    case 0x17:
      MIBLEsensors[_slot].NMT = _beacon.NMT;
      MIBLEsensors[_slot].eventType.NMT = 1;
      MI32.mode.shallTriggerTele = 1;
      // AddLog_P(LOG_LEVEL_DEBUG,PSTR("Mode 17: NMT: %u seconds"), _beacon.NMT);
    break;
#endif //USE_MI_DECRYPTION
    default:
      if (MIBLEsensors[_slot].type==NLIGHT){
        MIBLEsensors[_slot].eventType.motion = 1; //PIR
        MIBLEsensors[_slot].events++;
        MIBLEsensors[_slot].NMT = 0;
        MIBLEsensors[_slot].lastTime = millis();
        MI32.mode.shallTriggerTele = 1;
        // AddLog_P(LOG_LEVEL_DEBUG,PSTR("PIR: primary"),MIBLEsensors[_slot].lux );
      }
      else{
        AddLogBuffer(LOG_LEVEL_DEBUG,(uint8_t*)_buf,_bufSize);
      }
    break;
  }
  if(MIBLEsensors[_slot].eventType.raw == 0) return;
  MIBLEsensors[_slot].shallSendMQTT = 1;
  if(MI32.option.directBridgeMode) MI32.mode.shallTriggerTele = 1;
}

void MI32ParseATCPacket(const char * _buf, uint32_t length, const uint8_t *addr, int RSSI){
  ATCPacket_t *_packet = (ATCPacket_t*)_buf;
  uint32_t _slot = MIBLEgetSensorSlot(_packet->MAC, 0x0a1c, _packet->frameCnt); // This must be a hard-coded fake ID
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("%s at slot %u"), kMI32DeviceType[MIBLEsensors[_slot].type-1],_slot);
  if(_slot==0xff) return;

  MIBLEsensors[_slot].RSSI=RSSI;

  MIBLEsensors.at(_slot).temp = (float)(int16_t(__builtin_bswap16(_packet->temp)))/10.0f;
  MIBLEsensors.at(_slot).hum = (float)_packet->hum;
  MIBLEsensors[_slot].eventType.tempHum  = 1;
  MIBLEsensors.at(_slot).bat = _packet->batPer;
  MIBLEsensors[_slot].eventType.bat  = 1;

  MIBLEsensors[_slot].shallSendMQTT = 1;
  if(MI32.option.directBridgeMode) MI32.mode.shallTriggerTele = 1;

}

void MI32parseCGD1Packet(const char * _buf, uint32_t length, const uint8_t *addr, int RSSI){ // no MiBeacon
  uint8_t _addr[6];
  memcpy(_addr,addr,6);
  uint32_t _slot = MIBLEgetSensorSlot(_addr, 0x0576, 0); // This must be hard-coded, no object-id in Cleargrass-packet, we have no packet counter too
  AddLog_P(LOG_LEVEL_DEBUG,PSTR("%s at slot %u"), kMI32DeviceType[MIBLEsensors[_slot].type-1],_slot);
  if(_slot==0xff) return;
  MIBLEsensors[_slot].RSSI=RSSI;
  cg_packet_t _packet;
  memcpy((char*)&_packet,_buf,sizeof(_packet));
  switch (_packet.mode){
    case 0x0401:
      float _tempFloat;
      _tempFloat=(float)(_packet.temp)/10.0f;
      if(_tempFloat<60){
          MIBLEsensors.at(_slot).temp = _tempFloat;
          MIBLEsensors[_slot].eventType.temp  = 1;
          DEBUG_SENSOR_LOG(PSTR("CGD1: temp updated"));
      }
      _tempFloat=(float)(_packet.hum)/10.0f;
      if(_tempFloat<100){
          MIBLEsensors.at(_slot).hum = _tempFloat;
          MIBLEsensors[_slot].eventType.hum  = 1;
          DEBUG_SENSOR_LOG(PSTR("CGD1: hum updated"));
      }
      DEBUG_SENSOR_LOG(PSTR("CGD1: U16:  %x Temp U16: %x Hum"), _packet.temp,  _packet.hum);
      break;
    case 0x0102:
      if(_packet.bat<101){
      MIBLEsensors.at(_slot).bat = _packet.bat;
      MIBLEsensors[_slot].eventType.bat  = 1;
      DEBUG_SENSOR_LOG(PSTR("Mode a: bat updated"));
      }
      break;
    default:
      DEBUG_SENSOR_LOG(PSTR("MI32: unexpected CGD1-packet"));
  }
  if(MIBLEsensors[_slot].eventType.raw == 0) return;
  MIBLEsensors[_slot].shallSendMQTT = 1;
  if(MI32.option.directBridgeMode) MI32.mode.shallTriggerTele = 1;
}

void MI32ParseResponse(char *buf, uint16_t bufsize, const uint8_t* addr, int RSSI) {
    if(bufsize<9) {  //9 is from the NLIGHT
      return;
    }
    uint16_t _type= buf[3]*256 + buf[2];
    // AddLog_P(LOG_LEVEL_INFO, PSTR("%02x %02x %02x %02x"),(uint8_t)buf[0], (uint8_t)buf[1],(uint8_t)buf[2],(uint8_t)buf[3]);
    uint8_t _addr[6];
    memcpy(_addr,addr,6);
    uint16_t _slot = MIBLEgetSensorSlot(_addr, _type, buf[4]);
    if(_slot!=0xff) {
      MIBLEsensors[_slot].RSSI=RSSI;
      MI32parseMiBeacon(buf,_slot,bufsize);
    }
}

/**
 * @brief Parse a BLE advertisement packet
 *
 * @param payload
 * @param payloadLength
 * @param CID
 * @param SVC
 * @param UUID
 */
void MI32ParseGenericBeacon(const uint8_t* payload, size_t payloadLength, uint16_t* CID, uint16_t*SVC, uint16_t* UUID){
  AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("MI32: Beacon:____________"));
  for (uint32_t i = 0; i<payloadLength;){
    uint32_t ADtype = payload[i+1];
    uint32_t offset = payload[i];
    switch(ADtype){
      case 0x01:
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("Flags: %02x"), payload[i+2]);
        break;
      case 0x02: case 0x03:
        *UUID = payload[i+3]*256 + payload[i+2];
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("UUID: %04x"), *UUID);
        break;
      case 0x08: case 0x09:
      {
        char name[20];
        strncpy(name, (const char*)&payload[i+2], ((offset-1)< 19)?(offset-1):19);
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("Name: %s"), name);
      }
        break;
      case 0x0a:
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("TxPow: %02u"), payload[i+2]);
        break;
      case 0xff:
        *CID = payload[i+3]*256 + payload[i+2];
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("CID: %04x"), *CID);
        break;
      case 0x16:
        *SVC = payload[i+3]*256 + payload[i+2];
        AddLog_P(LOG_LEVEL_DEBUG_MORE,PSTR("SVC: %04x"), *SVC);
        break;
    }
    i+=offset+1;
  }
}

/**
 * @brief Handle a generic BLE advertisment in a running scan or to check a beacon
 *
 * @param payload
 * @param payloadLength
 * @param RSSI
 * @param addr
 */
void MI32HandleGenericBeacon(const uint8_t* payload, size_t payloadLength, int RSSI, const uint8_t* addr){
  if(MI32.state.beaconScanCounter==0){ //handle beacon
    for(auto &_beacon : MIBLEbeacons){
      if(memcmp(addr,_beacon.MAC,6)==0){
        MI32ParseGenericBeacon(payload,payloadLength,&_beacon.CID,&_beacon.SVC,&_beacon.UUID);
        _beacon.time = 0;
        _beacon.RSSI = RSSI;
        return;
      }
    }
    return;
  }
  // else handle scan
  if(MIBLEscanResult.size()>19) {
    AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: Scan buffer full"));
    MI32.state.beaconScanCounter = 1;
    return;
  }
  for(auto _scanResult : MIBLEscanResult){
    if(memcmp(addr,_scanResult.MAC,6)==0){
      // AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: known device"));
      return;
    }
  }
  scan_entry_t _new;
  _new.RSSI = RSSI;
  _new.CID = 0;
  _new.SVC = 0;
  _new.UUID = 0;
  memcpy(_new.MAC,addr,sizeof(_new.MAC));
  MI32ParseGenericBeacon(payload,payloadLength,&_new.CID,&_new.SVC,&_new.UUID);
  MIBLEscanResult.push_back(_new);
}


/**
 * @brief Add a beacon defined by its MAC-address, if only zeros are given, the beacon will be deactivated
 *
 * @param index 1-4 beacons are currently supported
 * @param data  null terminated char array representing a MAC-address in hex
 */
void MI32addBeacon(uint8_t index, char* data){
  auto &_new = MIBLEbeacons[index-1]; //TODO: check
  MI32HexStringToBytes(data,_new.MAC);
  char _MAC[18];
  ToHex_P(MIBLEbeacons[index-1].MAC,6,_MAC,18,':');
  char _empty[6] = {0};
  _new.time = 0;
  if(memcmp(_empty,_new.MAC,6) == 0){
    _new.active = false;
    AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: beacon%u deactivated"), index);
  }
  else{
    _new.active = true;
    MI32.mode.activeBeacon = 1;
    AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: beacon added with MAC: %s"), _MAC);
  }
}

/**
 * @brief Present BLE scan in the console, after that deleting the scan data
 *
 */
void MI32showScanResults(){
  size_t _size = MIBLEscanResult.size();
  ResponseAppend_P(PSTR(",\"BLEScan\":{\"Found\":%u,\"Devices\":["), _size);
  for(auto _scanResult : MIBLEscanResult){
    char _MAC[18];
    ToHex_P(_scanResult.MAC,6,_MAC,18,':');
    ResponseAppend_P(PSTR("{\"MAC\":\"%s\",\"CID\":\"0x%04x\",\"SVC\":\"0x%04x\",\"UUID\":\"0x%04x\",\"RSSI\":%d},"), _MAC, _scanResult.CID, _scanResult.SVC, _scanResult.UUID, _scanResult.RSSI);
  }
  if(_size != 0)TasmotaGlobal.mqtt_data[strlen(TasmotaGlobal.mqtt_data)-1] = 0; // delete last comma
  ResponseAppend_P(PSTR("]}"));
  MIBLEscanResult.clear();
  MI32.mode.shallShowScanResult = 0;
}

void MI32showBlockList(){
  ResponseAppend_P(PSTR(",\"Block\":["));
  for(auto _scanResult : MIBLEBlockList){
    char _MAC[18];
    ToHex_P(_scanResult.buf,6,_MAC,18,':');
    ResponseAppend_P(PSTR("\"%s\","), _MAC);
  }
  if(MIBLEBlockList.size()!=0) TasmotaGlobal.mqtt_data[strlen(TasmotaGlobal.mqtt_data)-1] = 0; // delete last comma
  ResponseAppend_P(PSTR("]"));
  MI32.mode.shallShowBlockList = 0;
}

bool MI32isInBlockList(const uint8_t* MAC){
  bool isBlocked = false;
  for(auto &_blockedMAC : MIBLEBlockList){
    if(memcmp(_blockedMAC.buf,MAC,6) == 0) isBlocked = true;
  }
  return isBlocked;
}

void MI32removeMIBLEsensor(uint8_t* MAC){
  MIBLEsensors.erase( std::remove_if( MIBLEsensors.begin() , MIBLEsensors.end(), [MAC]( mi_sensor_t _sensor )->bool
  { return (memcmp(_sensor.MAC,MAC,6) == 0); } 
  ), end( MIBLEsensors ) );
}
/***********************************************************************\
 * Read data from connections
\***********************************************************************/

void MI32notifyHT_LY(int slot, char *_buf, int len){
  DEBUG_SENSOR_LOG(PSTR("%s: raw data: %x%x%x%x%x%x%x"),D_CMND_MI32,_buf[0],_buf[1],_buf[2],_buf[3],_buf[4],_buf[5],_buf[6]);
  if(_buf[0] != 0 && _buf[1] != 0){
    memcpy(&LYWSD0x_HT,(void *)_buf,sizeof(LYWSD0x_HT));
    AddLog_P(LOG_LEVEL_DEBUG, PSTR("%s: T * 100: %u, H: %u, V: %u"),D_CMND_MI32,LYWSD0x_HT.temp,LYWSD0x_HT.hum, LYWSD0x_HT.volt);
    uint32_t _slot = slot;

    DEBUG_SENSOR_LOG(PSTR("MIBLE: Sensor slot: %u"), _slot);
    static float _tempFloat;
    _tempFloat=(float)(LYWSD0x_HT.temp)/100.0f;
    if(_tempFloat<60){
        MIBLEsensors[_slot].temp=_tempFloat;
        // MIBLEsensors[_slot].showedUp=255; // this sensor is real
    }
    _tempFloat=(float)LYWSD0x_HT.hum;
    if(_tempFloat<100){
      MIBLEsensors[_slot].hum = _tempFloat;
      DEBUG_SENSOR_LOG(PSTR("LYWSD0x: hum updated"));
    }
    MIBLEsensors[_slot].eventType.tempHum  = 1;
    if (MIBLEsensors[_slot].type == LYWSD03MMC || MIBLEsensors[_slot].type == MHOC401){
      MIBLEsensors[_slot].bat = ((float)LYWSD0x_HT.volt-2100.0f)/12.0f;
      MIBLEsensors[_slot].eventType.bat  = 1;
    }
    MIBLEsensors[_slot].shallSendMQTT = 1;
    MI32.mode.shallTriggerTele = 1;
  }
}


/**
 * @brief Launch functions from Core 1 to make race conditions less likely
 *
 */

void MI32Every50mSecond(){
  if(MI32.mode.shallTriggerTele){
      MI32.mode.shallTriggerTele = 0;
      MI32triggerTele();
  }
}

/**
 * @brief Main loop of the driver, "high level"-loop
 *
 */

void MI32EverySecond(bool restart){
  // read a battery if 
  // MI32.batteryreader.slot < filled and !MI32.batteryreader.active
  readOneBat();


  // read a sensor if 
  // MI32.sensorreader.slot < filled and !MI32.sensorreader.active
  // for sensors which need to get data through notify...
  readOneSensor();

  if (MI32.secondsCounter >= MI32.period){
    // kick off notification sensor reading every period.
    MI32.sensorreader.slot = 0;
    MI32.secondsCounter = 0;
  }

  static uint32_t _counter = MI32.period - 15;
  static uint32_t _nextSensorSlot = 0;

  for (uint32_t i = 0; i < MIBLEsensors.size(); i++) {
    if(MIBLEsensors[i].type==NLIGHT || MIBLEsensors[i].type==MJYD2S){
      MIBLEsensors[i].NMT++;
    }
  }

  uint32_t _idx = 0;
  uint32_t _activeBeacons = 0;
  for (auto &_beacon : MIBLEbeacons){
    _idx++;
    if(_beacon.active == false) continue;
    _activeBeacons++;
    _beacon.time++;
    Response_P(PSTR("{\"Beacon%u\":{\"Time\":%u}}"), _idx, _beacon.time);
    XdrvRulesProcess();
  }
  if(_activeBeacons==0) MI32.mode.activeBeacon = 0;

  if(MI32.state.beaconScanCounter!=0){
    MI32.state.beaconScanCounter--;
    if(MI32.state.beaconScanCounter==0){
      MI32.mode.shallShowScanResult = 1;
      MI32triggerTele();
    }
  }

  if(MI32.mode.shallShowStatusInfo == 1){
    MI32StatusInfo();
  }
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

void CmndMi32Period(void) {
  if (XdrvMailbox.data_len > 0) {
    if (1 == XdrvMailbox.payload) {
      MI32EverySecond(true);
    } else {
      MI32.period = XdrvMailbox.payload;
    }
  }
  ResponseCmndNumber(MI32.period);
}

void CmndMi32Time(void) {
  if (XdrvMailbox.data_len > 0) {
    if (MIBLEsensors.size() > XdrvMailbox.payload) {
      int res = genericTimeWriteFn(XdrvMailbox.payload);
      if (res > 0){
        AddLog_P(LOG_LEVEL_DEBUG, PSTR("MI32: will set Time"));
        ResponseCmndNumber(XdrvMailbox.payload);
      }
      if (res < 0) {
        AddLog_P(LOG_LEVEL_ERROR, PSTR("MI32: cannot set Time on sensor type"));
      }
      if (res == 0) {
        AddLog_P(LOG_LEVEL_ERROR, PSTR("MI32: cannot set Time right now"));
      }
    }
  }
}

void CmndMi32Page(void) {
  if (XdrvMailbox.payload > 0) {
    MI32.perPage = XdrvMailbox.payload;
  }
  ResponseCmndNumber(MI32.perPage);
}

// read ALL battery values where we can?
void CmndMi32Battery(void) {
  // trigger a read cycle
  MI32.batteryreader.slot = 0;
  ResponseCmndDone();
}

void CmndMi32Unit(void) {
  if (XdrvMailbox.data_len > 0) {
    if (MIBLEsensors.size() > XdrvMailbox.payload) {
      // TOGGLE unit?
      int res = genericUnitWriteFn(XdrvMailbox.payload, -1);
      if (res > 0){
        AddLog_P(LOG_LEVEL_DEBUG, PSTR("MI32: will toggle Unit"));
        ResponseCmndNumber(XdrvMailbox.payload);
        return;
      }
      if (res < 0) {
        AddLog_P(LOG_LEVEL_ERROR, PSTR("MI32: cannot toggle Unit on sensor type"));
      }
      if (res == 0) {
        AddLog_P(LOG_LEVEL_ERROR, PSTR("MI32: cannot toggle Unit right now"));
      }
    }
  }
}

#ifdef USE_MI_DECRYPTION
void CmndMi32Key(void) {
  if (44 == XdrvMailbox.data_len) {  // a KEY-MAC-string
    MI32AddKey(XdrvMailbox.data);
    ResponseCmndDone();
  }
}
#endif  // USE_MI_DECRYPTION

void CmndMi32Beacon(void) {
  if (XdrvMailbox.data_len == 0) {
    switch (XdrvMailbox.index) {
      case 0:
        MI32.state.beaconScanCounter = 8;
        ResponseCmndIdxChar(PSTR("Scanning..."));
        break;
      case 1: case 2: case 3: case 4:
        char _MAC[18];
        ResponseCmndIdxChar(ToHex_P(MIBLEbeacons[XdrvMailbox.index-1].MAC, 6, _MAC, 18, ':'));
        break;
    }
  } else {
    if ((12 == XdrvMailbox.data_len) || (17 == XdrvMailbox.data_len)) { // MAC-string without or with colons
      switch (XdrvMailbox.index) {
        case 1: case 2: case 3: case 4:
          MI32addBeacon(XdrvMailbox.index, XdrvMailbox.data);
          break;
      }
    }
    ResponseCmndIdxChar(XdrvMailbox.data);
  }
}

void CmndMi32Block(void){
  if (XdrvMailbox.data_len == 0) {
    switch (XdrvMailbox.index) {
      case 0:
        MIBLEBlockList.clear();
        // AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: size of ilist: %u"), MIBLEBlockList.size());
        ResponseCmndIdxChar(PSTR("block list cleared"));
        break;
      case 1:
        ResponseCmndIdxChar(PSTR("show block list"));
        break;  
    }
  }
  else {
    MAC_t _MACasBytes;
    MI32HexStringToBytes(XdrvMailbox.data,_MACasBytes.buf);
    switch (XdrvMailbox.index) {
      case 0:
        MIBLEBlockList.erase( std::remove_if( begin( MIBLEBlockList ), end( MIBLEBlockList ), [_MACasBytes]( MAC_t& _entry )->bool
          { return (memcmp(_entry.buf,_MACasBytes.buf,6) == 0); } 
          ), end( MIBLEBlockList ) );
        ResponseCmndIdxChar(PSTR("MAC not blocked anymore"));
        break;
      case 1:
        bool _notYetInList = true;
        for (auto &_entry : MIBLEBlockList) {
          if (memcmp(_entry.buf,_MACasBytes.buf,6) == 0){
            _notYetInList = false;
          }
        }
        if (_notYetInList) {
          MIBLEBlockList.push_back(_MACasBytes);
          ResponseCmndIdxChar(XdrvMailbox.data);
          MI32removeMIBLEsensor(_MACasBytes.buf);
        }
        // AddLog_P(LOG_LEVEL_INFO,PSTR("MI32: size of ilist: %u"), MIBLEBlockList.size());
        break;  
    }
  }
  MI32.mode.shallShowBlockList = 1;
  MI32triggerTele();
}

void CmndMi32Option(void){
  bool onOff = atoi(XdrvMailbox.data);
  switch(XdrvMailbox.index) {
    case 0:
      MI32.option.allwaysAggregate = onOff;
      break;
    case 1:
      MI32.option.noSummary = onOff;
      break;
    case 2:
      MI32.option.directBridgeMode = onOff;
      break;
  }
  ResponseCmndDone();
}

/*********************************************************************************************\
 * Presentation
\*********************************************************************************************/

const char HTTP_MI32[] PROGMEM = "{s}MI ESP32 v0917a{m}%u%s / %u{e}";
const char HTTP_MI32_MAC[] PROGMEM = "{s}%s %s{m}%s{e}";
const char HTTP_RSSI[] PROGMEM = "{s}%s " D_RSSI "{m}%d dBm{e}";
const char HTTP_BATTERY[] PROGMEM = "{s}%s" " Battery" "{m}%u %%{e}";
const char HTTP_LASTBUTTON[] PROGMEM = "{s}%s Last Button{m}%u {e}";
const char HTTP_EVENTS[] PROGMEM = "{s}%s Events{m}%u {e}";
const char HTTP_NMT[] PROGMEM = "{s}%s No motion{m}> %u seconds{e}";
const char HTTP_MI32_FLORA_DATA[] PROGMEM = "{s}%s" " Fertility" "{m}%u us/cm{e}";
const char HTTP_MI32_HL[] PROGMEM = "{s}<hr>{m}<hr>{e}";

void MI32Show(bool json)
{
  if (json) {
    if(MI32.mode.shallShowScanResult) {
      return MI32showScanResults();
    }
    else if(MI32.mode.shallShowBlockList) {
      return MI32showBlockList();
    }
#ifdef USE_HOME_ASSISTANT
    bool _noSummarySave = MI32.option.noSummary;
    bool _minimalSummarySave = MI32.option.minimalSummary;
    if(hass_mode==2){
      if(MI32.option.holdBackFirstAutodiscovery){
        if(!MI32.mode.firstAutodiscoveryDone){
          MI32.mode.firstAutodiscoveryDone = 1;
          return;
        }
      }
      MI32.option.noSummary = false;
      MI32.option.minimalSummary = false;
    }
#endif //USE_HOME_ASSISTANT

    if(!MI32.mode.triggeredTele){
      MI32.mode.shallClearResults=1;
      if(MI32.option.noSummary) return; // no message at TELEPERIOD
      }

    for (uint32_t i = 0; i < MIBLEsensors.size(); i++) {
      if(MI32.mode.triggeredTele && MIBLEsensors[i].eventType.raw == 0) continue;
      if(MI32.mode.triggeredTele && MIBLEsensors[i].shallSendMQTT==0) continue;

      ResponseAppend_P(PSTR(",\"%s-%02x%02x%02x\":"), // do not add the '{' now ...
        kMI32DeviceType[MIBLEsensors[i].type-1],
        MIBLEsensors[i].MAC[3], MIBLEsensors[i].MAC[4], MIBLEsensors[i].MAC[5]);

      uint32_t _positionCurlyBracket = strlen(TasmotaGlobal.mqtt_data); // ... this will be a ',' first, but later be replaced

      if((!MI32.mode.triggeredTele && !MI32.option.minimalSummary)||MI32.mode.triggeredTele){
        bool tempHumSended = false;
        if(MIBLEsensors[i].feature.tempHum){
          if(MIBLEsensors[i].eventType.tempHum || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate){
            if (!isnan(MIBLEsensors[i].hum) && !isnan(MIBLEsensors[i].temp)
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) {
              ResponseAppend_P(PSTR(","));
              ResponseAppendTHD(MIBLEsensors[i].temp, MIBLEsensors[i].hum);
              tempHumSended = true;
            }
          }
        }
        if(MIBLEsensors[i].feature.temp && !tempHumSended){
          if(MIBLEsensors[i].eventType.temp || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate) {
            if (!isnan(MIBLEsensors[i].temp)
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) {
              char temperature[FLOATSZ];
              dtostrfd(MIBLEsensors[i].temp, Settings.flag2.temperature_resolution, temperature);
              ResponseAppend_P(PSTR(",\"" D_JSON_TEMPERATURE "\":%s"), temperature);
            }
          }
        }
        if(MIBLEsensors[i].feature.hum && !tempHumSended){
          if(MIBLEsensors[i].eventType.hum || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate) {
            if (!isnan(MIBLEsensors[i].hum)
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) {
              char hum[FLOATSZ];
              dtostrfd(MIBLEsensors[i].hum, Settings.flag2.humidity_resolution, hum);
              ResponseAppend_P(PSTR(",\"" D_JSON_HUMIDITY "\":%s"), hum);
            }
          }
        }
        if (MIBLEsensors[i].feature.lux){
          if(MIBLEsensors[i].eventType.lux || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate){
            if (MIBLEsensors[i].lux!=0x0ffffff
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) { // this is the error code -> no lux
              ResponseAppend_P(PSTR(",\"" D_JSON_ILLUMINANCE "\":%u"), MIBLEsensors[i].lux);
#ifdef USE_HOME_ASSISTANT
              if (MIBLEsensors[i].lux==0x0ffffff) MI32nullifyEndOfMQTT_DATA();
#endif //USE_HOME_ASSISTANT
            }
          }
        }
        if (MIBLEsensors[i].feature.moist){
          if(MIBLEsensors[i].eventType.moist || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate){
            if (MIBLEsensors[i].moisture!=0xff
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) {
              ResponseAppend_P(PSTR(",\"" D_JSON_MOISTURE "\":%u"), MIBLEsensors[i].moisture);
#ifdef USE_HOME_ASSISTANT
              if (MIBLEsensors[i].moisture==0xff) MI32nullifyEndOfMQTT_DATA();
#endif //USE_HOME_ASSISTANT
            }
          }
        }
        if (MIBLEsensors[i].feature.fert){
          if(MIBLEsensors[i].eventType.fert || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate){
            if (MIBLEsensors[i].fertility!=0xffff
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
            ) {
              ResponseAppend_P(PSTR(",\"Fertility\":%u"), MIBLEsensors[i].fertility);
#ifdef USE_HOME_ASSISTANT
              if (MIBLEsensors[i].fertility==0xffff) MI32nullifyEndOfMQTT_DATA();
#endif //USE_HOME_ASSISTANT
            }
          }
        }
        if (MIBLEsensors[i].feature.Btn){
          if(MIBLEsensors[i].eventType.Btn
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode==2)
#endif //USE_HOME_ASSISTANT
          ){
            ResponseAppend_P(PSTR(",\"Btn\":%u"),MIBLEsensors[i].Btn);
          }
        }
      } // minimal summary
      if (MIBLEsensors[i].feature.PIR){
        if(MIBLEsensors[i].eventType.motion || !MI32.mode.triggeredTele){
          if(MI32.mode.triggeredTele) ResponseAppend_P(PSTR(",\"PIR\":1")); // only real-time
          ResponseAppend_P(PSTR(",\"Events\":%u"),MIBLEsensors[i].events);
        }
        else if(MIBLEsensors[i].eventType.noMotion && MI32.mode.triggeredTele){
          ResponseAppend_P(PSTR(",\"PIR\":0"));
        }
      }

      if (MIBLEsensors[i].type == FLORA && !MI32.mode.triggeredTele) {
        if (MIBLEsensors[i].firmware[0] != '\0') { // this is the error code -> no firmware
          ResponseAppend_P(PSTR(",\"Firmware\":\"%s\""), MIBLEsensors[i].firmware);
        }
      }

      if (MIBLEsensors[i].feature.NMT || !MI32.mode.triggeredTele){
        if(MIBLEsensors[i].eventType.NMT){
          ResponseAppend_P(PSTR(",\"NMT\":%u"), MIBLEsensors[i].NMT);
        }
      }
      if (MIBLEsensors[i].feature.bat){
        if(MIBLEsensors[i].eventType.bat || !MI32.mode.triggeredTele || MI32.option.allwaysAggregate){
          if (MIBLEsensors[i].bat != 0x00
#ifdef USE_HOME_ASSISTANT
              ||(hass_mode!=-1)
#endif //USE_HOME_ASSISTANT
          ) { // this is the error code -> no battery
          ResponseAppend_P(PSTR(",\"Battery\":%u"), MIBLEsensors[i].bat);
#ifdef USE_HOME_ASSISTANT
              if (MIBLEsensors[i].bat == 0x00) MI32nullifyEndOfMQTT_DATA();
#endif //USE_HOME_ASSISTANT
          }
        }
      }
      if (MI32.option.showRSSI) ResponseAppend_P(PSTR(",\"RSSI\":%d"), MIBLEsensors[i].RSSI);

      if(_positionCurlyBracket==strlen(TasmotaGlobal.mqtt_data)) ResponseAppend_P(PSTR(",")); // write some random char, to be overwritten in the next step
      ResponseAppend_P(PSTR("}"));
      TasmotaGlobal.mqtt_data[_positionCurlyBracket] = '{';
      MIBLEsensors[i].eventType.raw = 0;
      if(MIBLEsensors[i].shallSendMQTT==1){
        MIBLEsensors[i].shallSendMQTT = 0;
        continue;
      }
    }
    MI32.mode.triggeredTele = 0;
// add beacons
    uint32_t _idx = 0;
    for (auto _beacon : MIBLEbeacons){
      _idx++;
      if(!_beacon.active) continue;
      char _MAC[18];
      ToHex_P(_beacon.MAC,6,_MAC,18,':');
      ResponseAppend_P(PSTR(",\"Beacon%u\":{\"MAC\":\"%s\",\"CID\":\"0x%04x\",\"SVC\":\"0x%04x\","
                            "\"UUID\":\"0x%04x\",\"Time\":%u,\"RSSI\":%d}"),
                            _idx,_MAC,_beacon.CID,_beacon.SVC,_beacon.UUID,_beacon.time,_beacon.RSSI);
    }
#ifdef USE_HOME_ASSISTANT
    if(hass_mode==2){
      MI32.option.noSummary = _noSummarySave;
      MI32.option.minimalSummary = _minimalSummarySave;
    }
#endif //USE_HOME_ASSISTANT
#ifdef USE_WEBSERVER
    } else {
      static  uint16_t _page = 0;
      static  uint16_t _counter = 0;
      int32_t i = _page * MI32.perPage;
      uint32_t j = i + MI32.perPage;
      if (j+1>MIBLEsensors.size()){
        j = MIBLEsensors.size();
      }
      char stemp[5] ={0};
      if (MIBLEsensors.size()-(_page*MI32.perPage)>1 && MI32.perPage!=1) {
        sprintf_P(stemp,"-%u",j);
      }
      if (MIBLEsensors.size()==0) i=-1; // only for the GUI

      WSContentSend_PD(HTTP_MI32, i+1,stemp,MIBLEsensors.size());
      for (i; i<j; i++) {
        WSContentSend_PD(HTTP_MI32_HL);
        char _MAC[18];
        ToHex_P(MIBLEsensors[i].MAC,6,_MAC,18,':');
        WSContentSend_PD(HTTP_MI32_MAC, kMI32DeviceType[MIBLEsensors[i].type-1], D_MAC_ADDRESS, _MAC);
        WSContentSend_PD(HTTP_RSSI, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].RSSI);
        if (MIBLEsensors[i].type==FLORA) {
          if (!isnan(MIBLEsensors[i].temp)) {
            char temperature[FLOATSZ];
            dtostrfd(MIBLEsensors[i].temp, Settings.flag2.temperature_resolution, temperature);
            WSContentSend_PD(HTTP_SNS_TEMP, kMI32DeviceType[MIBLEsensors[i].type-1], temperature, TempUnit());
          }
          if (MIBLEsensors[i].moisture!=0xff) {
            WSContentSend_PD(HTTP_SNS_MOISTURE, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].moisture);
          }
          if (MIBLEsensors[i].fertility!=0xffff) {
            WSContentSend_PD(HTTP_MI32_FLORA_DATA, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].fertility);
          }
        }
        if (MIBLEsensors[i].type>FLORA) { // everything "above" Flora
          if (!isnan(MIBLEsensors[i].hum) && !isnan(MIBLEsensors[i].temp)) {
            WSContentSend_THD(kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].temp, MIBLEsensors[i].hum);
          }
        }
#ifdef USE_MI_DECRYPTION
        if (MIBLEsensors[i].type==NLIGHT || MIBLEsensors[i].type==MJYD2S) {
#else
        if (MIBLEsensors[i].type==NLIGHT) {
#endif //USE_MI_DECRYPTION
          WSContentSend_PD(HTTP_EVENTS, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].events);
          if(MIBLEsensors[i].NMT>0) WSContentSend_PD(HTTP_NMT, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].NMT);
        }
        if (MIBLEsensors[i].lux!=0x00ffffff) { // this is the error code -> no valid value
          WSContentSend_PD(HTTP_SNS_ILLUMINANCE, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].lux);
        }
        if(MIBLEsensors[i].bat!=0x00){
            WSContentSend_PD(HTTP_BATTERY, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].bat);
        }
        if (MIBLEsensors[i].type==YEERC){
          WSContentSend_PD(HTTP_LASTBUTTON, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].Btn);
        }
      }
      _counter++;
      if(_counter>3) {
        _page++;
        _counter=0;
      }
      if (MIBLEsensors.size()%MI32.perPage==0 && _page==MIBLEsensors.size()/MI32.perPage) { _page = 0; }
      if (_page>MIBLEsensors.size()/MI32.perPage) { _page = 0; }

    //always at the bottom of the page
    uint32_t _idx=0;
    if(MI32.mode.activeBeacon){
      WSContentSend_PD(HTTP_MI32_HL);
      char _sbeacon[] = "Beacon1";
      for (auto &_beacon : MIBLEbeacons){
        _idx++;
        if(!_beacon.active) continue;
        WSContentSend_PD(HTTP_MI32_HL);
        _sbeacon[6] = _idx + 0x30;
        char _MAC[18];
        ToHex_P(_beacon.MAC,6,_MAC,18,':');
        WSContentSend_PD(HTTP_MI32_MAC, _sbeacon, D_MAC_ADDRESS, _MAC);
        WSContentSend_PD(HTTP_RSSI, _sbeacon, _beacon.RSSI);
        if(_beacon.CID!=0) WSContentSend_PD(PSTR("{s}Beacon%u CID{m}0x%04X{e}"),_idx, _beacon.CID);
        if(_beacon.SVC!=0) WSContentSend_PD(PSTR("{s}Beacon%u SVC{m}0x%04X{e}"),_idx, _beacon.SVC);
        if(_beacon.UUID!=0) WSContentSend_PD(PSTR("{s}Beacon%u UUID{m}0x%04X{e}"),_idx, _beacon.UUID);
        WSContentSend_PD(PSTR("{s}Beacon%u Time{m}%u seconds{e}"),_idx, _beacon.time);
      }
    }
#endif  // USE_WEBSERVER
    }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xsns62(uint8_t function)
{
  if (!Settings.flag5.mi32_enable) { return false; }  // SetOption115 - Enable ESP32 MI32 BLE
  return false;

  bool result = false;

  switch (function) {
    case FUNC_INIT:
      MI32Init();
      break;
    case FUNC_EVERY_50_MSECOND:
      MI32Every50mSecond();
      break;
    case FUNC_EVERY_SECOND:
      MI32EverySecond(false);
      break;
    case FUNC_COMMAND:
      result = DecodeCommand(kMI32_Commands, MI32_Commands);
      break;
    case FUNC_JSON_APPEND:
      MI32Show(1);
      break;
#ifdef USE_WEBSERVER
    case FUNC_WEB_SENSOR:
      MI32Show(0);
      break;
#endif  // USE_WEBSERVER
    }
  return result;
}
#endif  // USE_MI_ESP32
#endif  // ESP32

#endif