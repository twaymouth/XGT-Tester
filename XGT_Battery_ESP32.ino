#include "driver/uart.h"

#define rxPin 1    //rx pin for serial 1 used to communicate with battery
#define txPin 2    //tx pin for serial 1 used to communicate with battery
#define testBtn 9  // button used to start test, pulls input low when pressed

//variables to store retrieved data
uint16_t battHealth = 0;
uint16_t cellSize = 0;
uint16_t parallelCnt = 0;
uint16_t charge = 0;
uint16_t numCharges = 0;
float temperature = 0;
float temperature1 = 0;
uint8_t lockStatus = 0;
float packVoltage = 0;
float cellVoltages[10] = { 0 };
char model[10] = { 0 };

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

int8_t sendBattery(uint8_t *buf, uint8_t *command, uint8_t cmdLength, uint8_t *rxLength) {  //send data to battery
  *rxLength = 0;
  uint8_t attempts = 0;
  while (*rxLength == 0 && attempts <= 1) {  //allow 2x attempts for each command though battery almost always responds first time.
    if (attempts > 0) { delay(50); };
    attempts++;
    Serial1.write(command, cmdLength);                     //write command to battery
    uart_wait_tx_done(UART_NUM_1, 500);                    //wait until tx buffer is empty, timeout of 500 should never be reached unless there is some sort of hardware error.
    uart_flush(UART_NUM_1);                                //flush uart buffer
    *rxLength = uart_read_bytes(UART_NUM_1, buf, 32, 15);  //takes battery around 10ms to respond to a command only reading max of 32 byte response as this is all that is needed however battery can technically send up to 256 bytes
  }

  if (*rxLength < 8) {
    return 1;  //got less than minimum packet size (8 bytes) return 1 signaling rx error,
  }

  for (uint8_t i = 0; i < *rxLength; ++i) {
    buf[i] = (lookup[buf[i] & 0b1111] << 4) | lookup[buf[i] >> 4];  //change bit order of recieved data from MSB first to LSB first,
  }

  uint8_t crc = checkCRC(buf, *rxLength);  //check CRC
  if (!crc) {
    Serial.println("CRC");
    return -1;  //return -1 signaling CRC error.
  }
  return 0;  //return 0 no error (:
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

bool getData() {  //get required data from battery
  uint8_t rxLength = 0;
  int8_t error = 0;
  uint8_t buffer[32] = { 0 };
  memset(model, 0, sizeof(model));
  Serial1.write(0x0);  //write 0x0 to wake battery (not sure if actually needed but whatever, does not seem to cause issues)
  delay(70);

  for (uint8_t i = 0; i < 11; i++) {  //loop through 8 commands defined above to get required data
    switch (i) {
      case 0:
        {
          error += sendBattery(buffer, modelCmd, sizeof(modelCmd), &rxLength);  //send requested command to battery, if this succceds should return 0;
          rxLength -= ((buffer[3] & 0xF) + 3);                                  //remove number of trailing FF bytes and CRC bytes and one extra becuase array is 0 based i.e. 32 bytes recieved = array locations 0 to 31.
          for (int i = 0; i < 8; i++) {
            sprintf(model + i, "%c", buffer[rxLength - i]);
          }
        }
        break;
      case 1:
        error += sendBattery(buffer, numChargesCmd, sizeof(numChargesCmd), &rxLength);
        numCharges = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        break;
      case 2:
        error += sendBattery(buffer, cellSizeCmd, sizeof(cellSizeCmd), &rxLength);
        cellSize = buffer[5];
        break;
      case 3:
        error += sendBattery(buffer, parallelCntCmd, sizeof(parallelCntCmd), &rxLength);
        parallelCnt = buffer[4];
        break;
      case 4:
        error += sendBattery(buffer, battHealthCmd, sizeof(battHealthCmd), &rxLength);
        battHealth = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        battHealth = battHealth / (cellSize * parallelCnt);
        break;
      case 5:
        error += sendBattery(buffer, chargeCmd, sizeof(chargeCmd), &rxLength);
        charge = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        charge = charge / 255;
        break;
      case 6:
        error += sendBattery(buffer, temperatureSens1Cmd, sizeof(temperatureSens1Cmd), &rxLength);
        temperature = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        temperature = -30 + ((temperature - 2431) / 10);
        break;
      case 7:
        error += sendBattery(buffer, temperatureSens2Cmd, sizeof(temperatureSens2Cmd), &rxLength);
        temperature1 = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        temperature1 = -30 + ((temperature1 - 2431) / 10);
        break;
      case 8:
        error += sendBattery(buffer, packVoltageCmd, sizeof(packVoltageCmd), &rxLength);
        packVoltage = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        packVoltage = packVoltage / 1000;
        break;
      case 9:
        for (int j = 1; j <= 10; j++) {  //get cell voltages, better not to try and understand what is going on here.
          cellVoltagesCmds[4] = (lookup[j * 2 & 0b1111] << 4) | (lookup[j * 2 >> 4]);
          cellVoltagesCmds[1] = (lookup[j * 2 + 194 & 0b1111] << 4) | (lookup[j * 2 + 194 >> 4]);
          error += sendBattery(buffer, cellVoltagesCmds, 8, &rxLength);
          cellVoltages[j - 1] = (__builtin_bswap16((buffer[4] << 8) | buffer[5])) / 1000.0;
        }
        break;
      case 10:
        error += sendBattery(buffer, lockStatusCmd, sizeof(lockStatusCmd), &rxLength);
        lockStatus = buffer[4];

        Serial.println();
        //packVoltage = __builtin_bswap16((buffer[4] << 8) | buffer[5]);
        //packVoltage = packVoltage / 1000;


        break;
    }
  }
  return error;  //this should be 0 if all commands succeeded.
}

void setup() {
  // put your setup code here, to run once:
  Serial1.begin(9600, SERIAL_8E1, rxPin, txPin, true);
  Serial.begin(115200);
  Serial.println("Press button to test battery, hold button for >5 seconds to attempt factory reset");
  Serial.println();
  pinMode(testBtn, INPUT_PULLUP);
}

void loop() {
  buttonState = digitalRead(testBtn);
  if (buttonState != lastButtonState) {
    if (buttonState == LOW) {  // Button is pressed (LOW when using INPUT_PULLUP)
      pressStartTime = millis();     
    } else {  // Button is released (HIGH when using INPUT_PULLUP)
      holdDuration = millis() - pressStartTime;
      if (holdDuration < 5000) {
        char buffer[200] = { 0 };                                                                                                                                                                                                                         //buffer to store string built from recievd data
        if (!getData()) {                                                                                                                                                                                                                                 //get data from battery, if this is ok should return 0.
          sprintf(buffer, "Model %s\n\nCharge Count %d\nHealth %d%%\nCharge %d%%\nTemp Sens 1 %.1f\nTemp Sens 2 %.1f\nLockout Status %d\nPack Voltage %.1f v\n", model, numCharges, battHealth, charge, temperature, temperature1, lockStatus, packVoltage);  //build big ugly char buffer with recievd data
          Serial.println(buffer);                                                                                                                                                                                                                         // print buffer
          memset(buffer, 0, sizeof(buffer));
          for (uint8_t i = 0; i < 10; i++) {  //loop through array of cell voltages printing each one.
            sprintf(buffer, "Cell %d - %.1f v", i + 1, cellVoltages[i]);
            Serial.println(buffer);
          }
        } else {  //if not 0 throw error,
          Serial.println("Error Communicating With Battery");
        }
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
