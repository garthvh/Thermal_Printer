//
// Thermal_Printer Arduino library
//
// Copyright (c) 2020 BitBank Software, Inc.
// Written by Larry Bank (bitbank@pobox.com)
// Project started 1/6/2020
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <Arduino.h>
#include <BLEDevice.h>
#include "Thermal_Printer.h"

static BLEUUID SERVICE_UUID("49535343-FE7D-4AE5-8FA9-9FAFD205E455");
static BLEUUID CHAR_UUID_DATA ("49535343-8841-43F4-A8D4-ECBE34729BB3");
static char Scanned_BLE_Name[32];
static String Scanned_BLE_Address;
static BLEScanResults foundDevices;
static BLEAddress *Server_BLE_Address;
static BLERemoteCharacteristic* pRemoteCharacteristicData;
static BLEScan *pBLEScan;
static BLEClient* pClient;
static char szPrinterName[32];
static int bb_width, bb_height; // back buffer width and height in pixels
static int tp_wrap, bb_pitch;
static int iCursorX = 0;
static int iCursorY = 0;
static uint8_t *pBackBuffer = NULL;
static uint8_t bConnected = 0;
extern "C" {
extern unsigned char ucFont[], ucBigFont[];
};

// Called for each device found during a BLE scan by the client
class tpAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
{

    void onResult(BLEAdvertisedDevice advertisedDevice)
    {
      Serial.printf("Scan Result: %s \n", advertisedDevice.toString().c_str());
      if (strcmp(advertisedDevice.getName().c_str(), szPrinterName) == 0)
      { // this is what we want
        Serial.println("A match!");
        Server_BLE_Address = new BLEAddress(advertisedDevice.getAddress());
        Scanned_BLE_Address = Server_BLE_Address->toString().c_str();
        strcpy(Scanned_BLE_Name, advertisedDevice.getName().c_str());
        Serial.println((char *)Scanned_BLE_Address.c_str());
        Serial.println(Scanned_BLE_Name);
      }
    }
}; // class tpAdvertisedDeviceCallbacks

//
// Provide a back buffer for your printer graphics
// This allows you to manage the RAM used on
// embedded platforms like Arduinos
// The memory is laid out horizontally (384 pixels across = 48 bytes)
// So a 384x384 buffer would need to be 48x384 = 18432 bytes
//
void tpSetBackBuffer(uint8_t *pBuffer, int iWidth, int iHeight)
{
  pBackBuffer = pBuffer;
  bb_width = iWidth;
  bb_height = iHeight;
  bb_pitch = (iWidth + 7) >> 3;
} /* tpSetBackBuffer() */

//
// Fill the frame buffer with a byte pattern
// e.g. all off (0x00) or all on (0xff)
//
void tpFill(unsigned char ucData)
{
  if (pBackBuffer != NULL)
    memset(pBackBuffer, ucData, bb_pitch * bb_height);
} /* tpFill() */
//
// Turn text wrap on or off for the oldWriteString() function
//
void tpSetTextWrap(int bWrap)
{
  tp_wrap = bWrap;
} /* tpSetTextWrap() */
//
// Invert font data
//
static void InvertBytes(uint8_t *pData, uint8_t bLen)
{
uint8_t i;
   for (i=0; i<bLen; i++)
   {
      *pData = ~(*pData);
      pData++;
   }
} /* InvertBytes() */

//
// Draw text into the graphics buffer
//
int tpDrawText(int x, int y, char *szMsg, int iFontSize, int bInvert)
{
int i, ty, iFontOff;
unsigned char c, *s, *d, ucTemp[64];

    if (x == -1 || y == -1) // use the cursor position
    {
      x = iCursorX; y = iCursorY;
    }
    else
    {
      iCursorX = x; iCursorY = y; // set the new cursor position
    }
    if (iCursorX >= bb_width || iCursorY >= bb_height-7)
       return -1; // can't draw off the display

    if (iFontSize == FONT_SMALL) // 8x8 font
    {
       i = 0;
       while (iCursorX < bb_width && szMsg[i] != 0 && iCursorY < bb_height)
       {
          c = (unsigned char)szMsg[i];
          iFontOff = (int)(c-32) * 8;
          memcpy(ucTemp, &ucFont[iFontOff], 8);
          if (bInvert) InvertBytes(ucTemp, 8);
          d = &pBackBuffer[(iCursorY * bb_pitch) + iCursorX/8];
          for (ty=0; ty<8; ty++)
          {
             d[0] = ucTemp[ty];
             d += bb_pitch;
          }
          iCursorX += 8;
          if (iCursorX >= bb_width && tp_wrap) // word wrap enabled?
          {
             iCursorX = 0; // start at the beginning of the next line
             iCursorY +=8;
          }
       i++;
       } // while
    return 0;
    } // 8x8
    else if (iFontSize == FONT_LARGE) // 16x32 font
    {
      i = 0;
      while (iCursorX < bb_width && iCursorY < bb_height-31 && szMsg[i] != 0)
      {
          s = (unsigned char *)&ucBigFont[(unsigned char)(szMsg[i]-32)*64];
          memcpy(ucTemp, s, 64);
          if (bInvert) InvertBytes(ucTemp, 64);
          d = &pBackBuffer[(iCursorY * bb_pitch) + iCursorX/8];
          for (ty=0; ty<32; ty++)
          {
             d[0] = s[0];
             d[1] = s[1];
             s += 2; d += bb_pitch;
          }
          iCursorX += 16;
          if (iCursorX >= bb_width && tp_wrap) // word wrap enabled?
          {
             iCursorX = 0; // start at the beginning of the next line
             iCursorY += 32;
          }
          i++;
       } // while
       return 0;
    } // 16x32
   return -1;
} /* tpDrawText() */
//
// Set (or clear) an individual pixel
//
int tpSetPixel(int x, int y, uint8_t ucColor)
{
uint8_t *d, mask;

  if (pBackBuffer == NULL)
     return -1;
  d = &pBackBuffer[(bb_pitch * y) + (x >> 3)];
  mask = 0x80 >> (x & 7);
  if (ucColor)
     d[0] |= mask;
  else
     d[0] &= ~mask;  
  return 0;
} /* tpSetPixel() */
//
// After a successful scan, connect to the printer
// returns 1 if successful, 0 for failure
//
int tpConnect(void)
{
    pClient  = BLEDevice::createClient();
//    Serial.printf(" - Created client, connecting to %s", Scanned_BLE_Address.c_str());

    // Connect to the BLE Server.
    pClient->connect(*Server_BLE_Address);
//    if (!pClient->isConnected())
//    {
//      Serial.println("Connect failed");
//      return false;
//    }
    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
    if (pRemoteService != NULL)
    {
//      Serial.println(" - Found our service");
      if (pClient->isConnected())
      {
        pRemoteCharacteristicData = pRemoteService->getCharacteristic(CHAR_UUID_DATA);
        if (pRemoteCharacteristicData != NULL)
        {
//          Serial.println("Got data transfer characteristic!");
          bConnected = 1;
          return 1;
        }
      } // if connected
    } // if service found
    else
    {
        bConnected = 0;
//      Serial.println("data Service not found");
    }
  return 0;
} /* tpConnect() */

void tpDisconnect(void)
{
   if (bConnected && pClient != NULL)
   {
      pClient->disconnect();
      bConnected = 0;
   }
} /* tpDisconnect() */

//
// Scan for compatible printers
// returns true if found
// and stores the printer address internally
// for use with the tpConnect() function
// iSeconds = how many seconds to scan for devices
//
int tpScan(const char *szName, int iSeconds)
{
unsigned long ulTime;

    strcpy(szPrinterName, szName);
    BLEDevice::init("ESP32");
    pBLEScan = BLEDevice::getScan(); //create new scan
    if (pBLEScan != NULL)
    {
      pBLEScan->setAdvertisedDeviceCallbacks(new tpAdvertisedDeviceCallbacks()); //Call the class that is defined above
      pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
      bConnected = false;
      Server_BLE_Address = NULL;
      pBLEScan->start(iSeconds); //Scan for N seconds
    }
    ulTime = millis();
    while (Server_BLE_Address == NULL && (millis() - ulTime) < iSeconds*1000L)
    {
       if (strcmp(Scanned_BLE_Name,szPrinterName) == 0) // found a device we want
       {
//         Serial.println("Found Device :-)");
           pBLEScan->stop(); // stop scanning
       }
       else
       {
          delay(10); // if you don't add this, the ESP32 will reset due to watchdog timeout
       }
    }
    return (Server_BLE_Address != NULL);
} /* tpScan() */
//
// Write data to the printer over BLE
//
static void tpWriteData(uint8_t *pData, int iLen)
{
   if (!bConnected || pRemoteCharacteristicData == NULL)
      return;
   pRemoteCharacteristicData->writeValue(pData, iLen);
} /* tpWriteData() */

//
// Send the graphics to the printer (must be connected over BLE first)
//
void tpPrintBuffer(void)
{
uint8_t *s, ucTemp[8];
int y;

  if (!bConnected)
    return;
// The printer command for graphics is laid out like this:
// 0x1d 'v' '0' '0' xLow xHigh yLow yHigh <x/8 * y data bytes>
  ucTemp[0] = 0x1d; ucTemp[1] = 'v';
  ucTemp[2] = '0'; ucTemp[3] = '0';
  ucTemp[4] = (bb_width+7)>>3; ucTemp[5] = 0;
  ucTemp[6] = (uint8_t)bb_height; ucTemp[7] = (uint8_t)(bb_height >> 8);
  tpWriteData(ucTemp, 8);
// Now write the graphics data
  s = pBackBuffer;
  for (y=0; y<bb_height; y++)
  {
    tpWriteData(s, bb_pitch);
    delay(1+(bb_pitch/8)); // need to delay the data a little or it will overwhelm the printer
    s += bb_pitch;
  }
} /* tpPrintBuffer() */
//
// Draw a line between 2 points
//
void tpDrawLine(int x1, int y1, int x2, int y2, uint8_t ucColor)
{
  int temp;
  int dx = x2 - x1;
  int dy = y2 - y1;
  int error;
  uint8_t *p, mask;
  int xinc, yinc;

  if (x1 < 0 || x2 < 0 || y1 < 0 || y2 < 0 || x1 >= bb_width || x2 >= bb_width || y1 >= bb_height || y2 >= bb_height)
     return;

  if(abs(dx) > abs(dy)) {
    // X major case
    if(x2 < x1) {
      dx = -dx;
      temp = x1;
      x1 = x2;
      x2 = temp;
      temp = y1;
      y1 = y2;
      y2 = temp;
    }

    dy = (y2 - y1);
    error = dx >> 1;
    yinc = 1;
    if (dy < 0)
    {
      dy = -dy;
      yinc = -1;
    }
    p = &pBackBuffer[(y1 * bb_pitch) + (x1 >> 3)]; // point to current spot in back buffer
    mask = 0x80 >> (x1 & 7); // current bit offset
    for(; x1 <= x2; x1++) {
      if (ucColor)
        *p |= mask; // set pixel and increment x pointer
      else
        *p &= ~mask;
      mask >>= 1;
      if (mask == 0) {
         mask = 0x80;
         p++;
      }
      error -= dy;
      if (error < 0)
      {
        error += dx;
        if (yinc > 0)
           p += bb_pitch;
        else
           p -= bb_pitch;
      }
    } // for x1
  }
  else {
    // Y major case
    if(y1 > y2) {
      dy = -dy;
      temp = x1;
      x1 = x2;
      x2 = temp;
      temp = y1;
      y1 = y2;
      y2 = temp;
    }

    p = &pBackBuffer[(y1 * bb_pitch) + (x1 >> 3)]; // point to current spot in back buffer
    mask = 0x80 >> (x1 & 7); // current bit offset
    dx = (x2 - x1);
    error = dy >> 1;
    xinc = 1;
    if (dx < 0)
    {
      dx = -dx;
      xinc = -1;
    }
    for(; y1 <= y2; y1++) {
      if (ucColor)
         *p |= mask; // set the pixel
      else
         *p &= ~mask;
      p += bb_pitch; // y++
      error -= dx;
      if (error < 0)
      {
        error += dy;
        x1 += xinc;
        if (xinc > 0)
        {
          mask >>= 1;
          if (mask == 0) // change the byte
          {
             p++;
             mask = 0x80;
          }
        } // positive delta x
        else // negative delta x
        {
          mask <<= 1;
          if (mask == 0)
          {
             p--;
             mask = 1;
          }
        }
      }
    } // for y
  } // y major case
} /* tpDrawLine() */
