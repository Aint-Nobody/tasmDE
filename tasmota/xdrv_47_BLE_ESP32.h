/*
  xdrv_47_BLE_ESP32.h - BLE via ESP32 support for Tasmota

  Copyright (C) 2020  Christian Baars and Theo Arends and Simon Hailes

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
*/

/*
  xdrv_46:
  This driver uses the ESP32 BLE functionality to hopefully provide enough
  BLE functionality to implement specific drivers on top of it.

  As a generic driver, it can:
    Be asked to 
      connect/write to a MAC/Service/Characteristic
      connect/read from a MAC/Service/Characteristic
      connect/write/awaitnotify from a MAC/Service/Characteristic/NotifyCharacteristic
      connect/read/awaitnotify from a MAC/Service/Characteristic/NotifyCharacteristic

    Cmnds:
      BLEOp0 - requests status of operations
      BLEOp1 MAC - create an operation in preparation, and populate it's MAC address
      BLEOp2 Service - add a serviceUUID to the operation in preparation
      BLEOp3 Characteristic - add a CharacteristicUUID to the operation in preparation for read/write
      BLEOp4 writedata - optional:add data to write to the operation in preparation - hex string
      BLEOp5 - optional:signify that a read should be done
      BLEOp6 NotifyCharacteristic - optional:add a NotifyCharacteristicUUID to the operation in preparation to wait for a notify
      BLEOp9 - publish the 'operation in preparation' to MQTT.
      BLEOp10 - add the 'operation in preparation' to the queue of operations to perform.

  Other drivers can add callbacks to receive advertisments
  Other drivers can add 'operations' to be performed and receive callbacks from the operation's success or failure

Example:
Write and request next notify:
backlog BLEOp1 001A22092EE0; BLEOp2 3e135142-654f-9090-134a-a6ff5bb77046; BLEOp3 3fa4585a-ce4a-3bad-db4b-b8df8179ea09; BLEOp4 03; BLEOp6 d0e8434d-cd29-0996-af41-6c90f4e0eb2a;
BLEOp10 -> 
19:25:08 RSL: tele/tasmota_E89E98/SENSOR = {"BLEOperation":{"opid":"3,"state":"1,"MAC":"001A22092EE0","svc":"3e135142-654f-9090-134a-a6ff5bb77046","char":"3fa4585a-ce4a-3bad-db4b-b8df8179ea09","wrote":"03}}
19:25:08 queued 0 sent {"BLEOperation":{"opid":"3,"state":"1,"MAC":"001A22092EE0","svc":"3e135142-654f-9090-134a-a6ff5bb77046","char":"3fa4585a-ce4a-3bad-db4b-b8df8179ea09","wrote":"03}}
19:25:08 RSL: stat/tasmota_E89E98/RESULT = {"BLEOp":"Done"}
.....
19:25:11 RSL: tele/tasmota_E89E98/SENSOR = {"BLEOperation":{"opid":"3,"state":"11,"MAC":"001A22092EE0","svc":"3e135142-654f-9090-134a-a6ff5bb77046","char":"3fa4585a-ce4a-3bad-db4b-b8df8179ea09","wrote":"03","notify":"020109000428}}

state: 1 -> starting, 
7 -> read complete
8 -> write complete 
11 -> notify complete
0x100 + -> failure (see GEN_STATE_FAILED_XXXX constants below.)


The driver can also be used by other drivers, using the functions:

void registerForAdvertismentCallbacks(char *loggingtag, ADVERTISMENT_CALLBACK* pFn);
void registerForOpCallbacks(char *loggingtag, OPCOMPLETE_CALLBACK* pFn);
bool extQueueOperation(generic_sensor_t** op);

These allow other code to
  receive advertisments
  receive operation callbacks.
  create and start an operation, and get a callback when done/failed.

i.e. the Bluetooth of the ESP can be shared without conflict.

*/


#ifndef BLE_ESP32_H
#define BLE_ESP32_H

#ifdef ESP32                       // ESP32 only. Use define USE_HM10 for ESP8266 support

#ifdef USE_BLE_ESP32


#include <NimBLEDevice.h>
#include <NimBLEAdvertisedDevice.h>
#include "NimBLEEddystoneURL.h"
#include "NimBLEEddystoneTLM.h"
#include "NimBLEBeacon.h"

namespace BLE_ESP32 {
// generic sensor type used as during
// connect/read/wrtie/notify operations
// only one operation will happen at a time 

#pragma pack( push, 0 )  // aligned structures for speed.  but be sepcific

/////////////////////////////////////////////////////
// states for runTaskDoneOperation
#define GEN_STATE_IDLE 0
#define GEN_STATE_START 1

#define GEN_STATE_READDONE 7
#define GEN_STATE_WRITEDONE 8
#define GEN_STATE_WAITNOTIFY 9
#define GEN_STATE_WAITINDICATE 10

#define GEN_STATE_NOTIFIED 11


// Errors are all base on 0x100
#define GEN_STATE_FAILED 0x100
#define GEN_STATE_FAILED_CANTNOTIFYORINDICATE 0x101
#define GEN_STATE_FAILED_CANTREAD 0x102
#define GEN_STATE_FAILED_CANTWRITE 0x103
#define GEN_STATE_FAILED_NOSERVICE 0x104
#define GEN_STATE_FAILED_NO_RW_CHAR 0x105
#define GEN_STATE_FAILED_NONOTIFYCHAR 0x106
#define GEN_STATE_FAILED_NOTIFYTIMEOUT 0x107
#define GEN_STATE_FAILED_READ 0x108
#define GEN_STATE_FAILED_WRITE 0x109
#define GEN_STATE_FAILED_CONNECT 0x10A
#define GEN_STATE_FAILED_NOTIFY 0x10B
#define GEN_STATE_FAILED_INDICATE 0x10C
#define GEN_STATE_FAILED_NODEVICE 0x10D
#define GEN_STATE_FAILED_NOREADWRITE 0x110
#define GEN_STATE_FAILED_CANCEL 0x111
//
/////////////////////////////////////////////////////


struct generic_sensor_t {
  uint16_t state;
  uint32_t opid; // incrementing id so we can find them
  
  // uint8_t cancel; 
  // uint8_t requestType; 
  char MAC[13];
  char serviceStr[100];
  char characteristicStr[100];
  char notificationCharacteristicStr[100];
  int RSSI;
  uint64_t notifytimer;
  uint8_t dataToWrite[100];
  uint8_t writelen;
  uint8_t dataRead[100];
  uint8_t readlen;
  uint8_t readtruncated;
  uint8_t dataNotify[100];
  uint8_t notifylen;
  uint8_t notifytruncated;

  NimBLEClient *pClient;


  // NOTE!!!: this callback is called DIRECTLY from the operation task, so be careful about cross-thread access of data
  // if is called after read, so that you can do a read/modify/write operation on a characteristic.
  // i.e. modify dataToWrite and writelen according to what you see in readData and readlen.
  // for a normal read, please use the OPCOMPLETE_CALLBACK 'completecallback'
  // normally null
  void *readmodifywritecallback; // READ_CALLBACK function, used by external drivers

  void *completecallback; // OPCOMPLETE_CALLBACK function, used by external drivers
  void *context; // opaque context, used by external drivers
};


////////////////////////////////////////////////////////////////
// structure for callbacks from other drivers from advertisments.
struct ble_advertisment_t {
  BLEAdvertisedDevice *advertisedDevice; // the full NimBLE advertisment, in case people need MORE info.

  const uint8_t *addr;
  int RSSI;
  const char *name;

  const uint8_t *payload;
  uint8_t payloadLen;

  const uint8_t *manufacturerData;
  uint8_t manufacturerDataLen;

  uint8_t svcdataCount;
  struct {
    const ble_uuid_any_t* serviceUUID;
    char serviceUUIDStr[40]; // longest UUID 36 chars?
    const uint8_t* serviceData;
    uint8_t serviceDataLen;
  } svcdata[5];
  uint8_t serviceCount;
  struct {
    const ble_uuid_any_t* serviceUUID;
    char serviceUUIDStr[40]; // longest UUID 36 chars?
  } services[5];
};

#pragma pack( pop )  // byte-aligned structures to read the sensor data

////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////
// External interface to this driver for use by others.
//
// callback types to be used by external drivers
//
// returns - 
//  0 = let others see this, 
//  1 = I processed this, no need to give it to the next callback
//  2 = I want this device erased from the scan
typedef int ADVERTISMENT_CALLBACK(BLE_ESP32::ble_advertisment_t *pStruct);
// returns - 0 = let others see this, 1 = I processed this, no need to give it to the next callback, or post on MQTT
typedef int OPCOMPLETE_CALLBACK(BLE_ESP32::generic_sensor_t *pStruct);

// NOTE!!!: this callback is called DIRECTLY from the operation task, so be careful about cross-thread access of data
// if is called after read, so that you can do a read/modify/write operation on a characteristic.
typedef int READ_CALLBACK(BLE_ESP32::generic_sensor_t *pStruct);

typedef int SCANCOMPLETE_CALLBACK(NimBLEScanResults results);

// tag is just a name for logging
void registerForAdvertismentCallbacks(const char *tag, BLE_ESP32::ADVERTISMENT_CALLBACK* pFn);
void registerForOpCallbacks(const char *tag, BLE_ESP32::OPCOMPLETE_CALLBACK* pFn);
void registerForScanCallbacks(const char *tag, BLE_ESP32::SCANCOMPLETE_CALLBACK* pFn);

int extQueueOperation(BLE_ESP32::generic_sensor_t** op);
//
///////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////
// utilities
// dump a binary to hex
char * dump(char *dest, int maxchars, uint8_t *src, int len);

}


#endif
#endif

#endif