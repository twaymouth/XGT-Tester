// 11/2026. Updated to remove dependency  on  "driver/uart.h" to ensure compatability across ESP32 core versions. Changed error handling behaviour so good values will be returned even if others have errors.

#define rxPin 1    //rx pin for serial 1 used to communicate with battery
#define txPin 2    //tx pin for serial 1 used to communicate with battery
#define testBtn 9  // button used to start test, pulls input low when pressed

//commands used to get said data
uint8_t modelCmd[] = { 0xA5, 0xA5, 0x0, 0x58, 0x0A, 0xD4, 0xB2, 0x32, 0x0, 0xD3, 0xC8, 0xE0, 0x0, 0x60, 0x0, 0xC0, 0x0, 0x80, 0xC8, 0xD0, 0x40, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  //command to get battery model
uint8_t numChargesCmd[] = { 0x33, 0xC8, 0x3, 0x0, 0x2A, 0x0, 0x0, 0xCC };                                                                                                                                            //command to get number of charges
uint8_t cellSizeCmd[] = { 0x33, 0x27, 0xBB, 0x10, 0x0, 0x0, 0x0, 0xCC };                                                                                                                                             //command to get size of cells used in pack constrution i.e. 2000 mAh
uint8_t parallelCntCmd[] = { 0x33, 0x67, 0xBB, 0x50, 0x0, 0x0, 0x0, 0xCC };                                                                                                                                          //command to get cells in parallel, i.e. 4ah pack would be 2 sets of cells in parallel
uint8_t battHealthCmd[] = { 0x33, 0xC4, 0x3, 0x0, 0x26, 0x0, 0x0, 0xCC };                                                                                                                                            //command to get estimated battery health (total capacity available)
uint8_t chargeCmd[] = { 0x33, 0x13, 0x3, 0x80, 0x10, 0x0, 0x0, 0xCC };                                                                                                                                               //command to get percentage change of pack
uint8_t temperatureSens1Cmd[] = { 0x33, 0x3B, 0x3, 0xC0, 0x58, 0x0, 0x0, 0xCC };
uint8_t temperatureSens2Cmd[] = { 0x33, 0x7B, 0x3, 0xC0, 0x38, 0x0, 0x0, 0xCC };  //command to get temperature of pack
uint8_t packVoltageCmd[] = { 0x33, 0x43, 0x3, 0xC0, 0x0, 0x0, 0x0, 0xCC };        //command to get pack voltage
uint8_t cellVoltagesCmds[] = { 0x33, 0x23, 0x03, 0xC0, 0x00, 0x0, 0x0, 0xCC };
uint8_t lockStatusCmd[] = { 0x33, 0xF8, 0x3, 0x0, 0x6, 0x0, 0x0, 0xCC };  //command to get cell voltages
uint8_t resetCmd[] = { 0x33, 0xC8, 0x9B, 0x69, 0xA5, 0x0, 0x0, 0xCC };
uint8_t resetCmd1[] = { 0x33, 0x0, 0x4B, 0xF4, 0x0, 0x0, 0x0, 0xCC };

static unsigned char lookup[16] = { 0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };  //lookup table to reverse bit order of recieved data (battery operates in MSB first bit order)

int buttonState = 0;
int lastButtonState = 1;
unsigned long pressStartTime = 0;
unsigned long holdDuration = 0;

bool checkCRC(uint8_t *rxBuf, uint8_t length) {  //calculate CRC based on data recieved from battery.
  uint16_t crc = 0;
  if (rxBuf[0] == 0xA5 && rxBuf[1] == 0xA5 || rxBuf[0] == 0xCC && rxBuf[7] == 0x33) {  //check cromulence of message before attempting CRC calculation
    if (rxBuf[0] == 0xCC) {                                                            //short message type
      crc += rxBuf[0];                                                                 //add first byte (0xCC)
      for (uint8_t i = 2; i < length; i++) {                                           //loop through array of data starting at position 3 to exclude CRC
        crc += rxBuf[i];                                                               //sum bytes in packet
      }
      crc = crc % 256;  // take modulo 256
      if (crc != rxBuf[1]) {
        return false;  //if calculated CRC does not match byte 1 (crc) return false;
      }
    } else {                                      //long message type
      length -= (rxBuf[3] & 0xF);                 //size of data recieved from battery - number of padding bytes
      for (uint8_t i = 2; i < length - 2; i++) {  //loop through array of data starting at position 3 to exclude A5A5 header and excluding final word which is CRC
        crc += rxBuf[i];
      }
      if ((rxBuf[length - 2] << 8 | rxBuf[length - 1]) != crc) {  //bitwise or last 2 bytes to compare calculated CRC with supplied CRC (rember array is 0 based and msgLength is 1 based)
        return false;
      }
    }
  }
  return true;  //return true, good CRC
}

int8_t sendBattery(uint8_t *buf, uint8_t *command, uint8_t cmdLength, uint8_t *rxLength) {
  *rxLength = 0;
  uint8_t attempts = 0;
  Serial1.setTimeout(50);  // 50ms timeout for battery response longer command.

  while (*rxLength == 0 && attempts <= 1) {  //1 retry attempt.
    if (attempts > 0) {
      delay(50);
    }
    attempts++;
    // Drain any stale bytes (like the 0x0 wakeup echo or trailing bytes >32)
    while (Serial1.available()) {
      Serial1.read();
    }
    // Write command and wait until all TX bits leave the pin
    Serial1.write(command, cmdLength);
    Serial1.flush();

    //clear any electrical backfeed (occurs during TX, ~10ms before battery replies)
    while (Serial1.available()) {
      Serial1.read();
    }
    // Read the actual battery response (up to 32 bytes)
    *rxLength = Serial1.readBytes(buf, 32);
    //Serial.println(*rxLength);
  }

  if (*rxLength < 8) {
    return 1;  // rx error
  }

  for (uint8_t i = 0; i < *rxLength; ++i) {
    buf[i] = (lookup[buf[i] & 0b1111] << 4) | lookup[buf[i] >> 4];
  }

  if (!checkCRC(buf, *rxLength)) {
    Serial.println("CRC");
    return -1;
  }
  return 0;
}
uint8_t reset() {
  uint8_t rxLength = 0;
  int8_t error = 0;
  uint8_t buffer[32] = { 0 };
  error += sendBattery(buffer, resetCmd, sizeof(resetCmd), &rxLength);
  delay(10);
  error += sendBattery(buffer, resetCmd1, sizeof(resetCmd1), &rxLength);
  return (error);
}

void getData() {
  uint8_t rxLength = 0;
  uint8_t buffer[32] = { 0 };
  uint16_t cellSize = 0;
  uint16_t parallelCnt = 0;
  char model[10] = { 0 };

  Serial1.write(0x0);  // Wake battery
  delay(70);

  for (uint8_t i = 0; i < 11; i++) {
    switch (i) {
      case 0:
        if (!sendBattery(buffer, modelCmd, sizeof(modelCmd), &rxLength)) {
          rxLength -= ((buffer[3] & 0xF) + 3);
          for (int k = 0; k < 8; k++) {
            model[k] = buffer[rxLength - k];
          }
          Serial.printf("Model %s\n\n", model);
        } else {
          Serial.println("Error getting model\n");
        }
        break;

      case 1:
        if (!sendBattery(buffer, numChargesCmd, sizeof(numChargesCmd), &rxLength)) {
          Serial.printf("Charge Count %d\n", __builtin_bswap16((buffer[4] << 8) | buffer[5]));
        } else {
          Serial.println("Error getting charge count");
        }
        break;

      case 2:
        if (!sendBattery(buffer, cellSizeCmd, sizeof(cellSizeCmd), &rxLength)) {
          cellSize = buffer[5];
        } else {
          Serial.println("Error getting cell size");
        }
        break;

      case 3:
        if (!sendBattery(buffer, parallelCntCmd, sizeof(parallelCntCmd), &rxLength)) {
          parallelCnt = buffer[4];
        } else {
          Serial.println("Error getting parallel count");
        }
        break;

      case 4:
        if (!sendBattery(buffer, battHealthCmd, sizeof(battHealthCmd), &rxLength)) {
          if (cellSize > 0 && parallelCnt > 0) {
            uint16_t health = __builtin_bswap16((buffer[4] << 8) | buffer[5]) / (cellSize * parallelCnt);
            Serial.printf("Health %d%%\n", health);
          } else {
            Serial.println("Error calculating health (missing cell info)");
          }
        } else {
          Serial.println("Error getting health");
        }
        break;

      case 5:
        if (!sendBattery(buffer, chargeCmd, sizeof(chargeCmd), &rxLength)) {
          Serial.printf("Charge %d%%\n", __builtin_bswap16((buffer[4] << 8) | buffer[5]) / 255);
        } else {
          Serial.println("Error getting charge");
        }
        break;

      case 6:
        if (!sendBattery(buffer, temperatureSens1Cmd, sizeof(temperatureSens1Cmd), &rxLength)) {
          float temp1 = -30.0 + ((__builtin_bswap16((buffer[4] << 8) | buffer[5]) - 2431) / 10.0);
          Serial.printf("Temp Sens 1 %.1f\n", temp1);
        } else {
          Serial.println("Error getting temp sensor 1");
        }
        break;

      case 7:
        if (!sendBattery(buffer, temperatureSens2Cmd, sizeof(temperatureSens2Cmd), &rxLength)) {
          float temp2 = -30.0 + ((__builtin_bswap16((buffer[4] << 8) | buffer[5]) - 2431) / 10.0);
          Serial.printf("Temp Sens 2 %.1f\n", temp2);
        } else {
          Serial.println("Error getting temp sensor 2");
        }
        break;

      case 8:
        if (!sendBattery(buffer, packVoltageCmd, sizeof(packVoltageCmd), &rxLength)) {
          float packVoltage = __builtin_bswap16((buffer[4] << 8) | buffer[5]) / 1000.0;
          Serial.printf("Pack Voltage %.1f v\n", packVoltage);
        } else {
          Serial.println("Error getting pack voltage");
        }
        break;

      case 9:
        for (int j = 1; j <= 10; j++) {
          cellVoltagesCmds[4] = (lookup[(j * 2) & 0b1111] << 4) | (lookup[(j * 2) >> 4]);
          cellVoltagesCmds[1] = (lookup[(j * 2 + 194) & 0b1111] << 4) | (lookup[(j * 2 + 194) >> 4]);
          if (!sendBattery(buffer, cellVoltagesCmds, 8, &rxLength)) {
            float cellV = __builtin_bswap16((buffer[4] << 8) | buffer[5]) / 1000.0;
            Serial.printf("Cell %d - %.1f v\n", j, cellV);
          } else {
            Serial.printf("Error getting cell %d voltage\n", j);
          }
        }
        break;

      case 10:
        if (!sendBattery(buffer, lockStatusCmd, sizeof(lockStatusCmd), &rxLength)) {
          Serial.printf("Lockout Status %d\n\n", buffer[4]);
        } else {
          Serial.println("Error getting lockout status\n");
        }
        break;
    }
  }
}

void setup() {
  Serial1.begin(9600, SERIAL_8E1, rxPin, txPin, true);
  Serial.begin(115200);
  Serial.println("Press button to test battery, hold button for >5 seconds to attempt factory reset");
  Serial.println();
  pinMode(testBtn, INPUT_PULLUP);//setup button as input pullup, change depending on your hardware config
}

void loop() {
  buttonState = digitalRead(testBtn);
  if (buttonState != lastButtonState) {
    if (buttonState == LOW) {  // Button is pressed (LOW when using INPUT_PULLUP)
      pressStartTime = millis();
    } else {  // Button is released (HIGH when using INPUT_PULLUP)
      holdDuration = millis() - pressStartTime;
      if (holdDuration < 5000) {
        getData();
        delay(1000);  //wait 1 second before testing again.
      } else if (holdDuration > 5000) {
        Serial.println("Attempting Reset");
        if (!reset()) {
          Serial.println("Battery accepted reset command");
        } else {
          Serial.println("Reset command returned error");
        }
      }
    }
    // Update lastButtonState for the next iteration
    lastButtonState = buttonState;
    delay(50);  // Small delay for debouncing
  }
}
