#include "weather_sensor.h"
#include <Wire.h>

// BMP280/BME280 registers
#define BMP280_ADDR 0x76
#define BMP280_REG_ID 0xD0
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG 0xF5
#define BMP280_REG_DATA 0xF7
#define BMP280_REG_CTRL_HUM 0xF2  // BME280 humidity control

// Compensation parameters
static uint16_t dig_T1;
static int16_t dig_T2;
static int16_t dig_T3;
static uint16_t dig_P1;
static int16_t dig_P2;
static int16_t dig_P3;
static int16_t dig_P4;
static int16_t dig_P5;
static int16_t dig_P6;
static int16_t dig_P7;
static int16_t dig_P8;
static int16_t dig_P9;
// BME280 humidity compensation (BMP280 doesn't have these)
static int16_t dig_H1;
static int16_t dig_H2;
static int16_t dig_H3;
static int16_t dig_H4;
static int16_t dig_H5;
static int16_t dig_H6;

static bool sensorFound = false;
static bool isBME280 = false;  // true = BME280 (has humidity), false = BMP280
BME280Data localWeather = {0, 0, 0, false};

bool weatherSensorFound() {
  return sensorFound;
}

// Known I2C devices on T-TWR Rev 2.1
static const char* i2cDeviceName(uint8_t addr) {
  switch (addr) {
    case 0x34: return "XPowersAXP2101 (PMU)";
    case 0x3C: return "OLED SH1106 (VHF)";
    case 0x3D: return "OLED SH1106 (UHF)";
    case 0x58: return "BMP280 (pressure/temp)";
    case 0x60: return "BME280 (pressure/temp/humidity)";
    case 0x68: return "DS3231 (RTC) or AXP2101 (alternate)";
    case 0x76: return "BMP280/BME280 (addr 0x76)";
    case 0x77: return "BMP280/BME280 (addr 0x77)";
    default:   return NULL;
  }
}

void scanI2C() {
  Serial.println("\nI2C Scan:");
  Serial.println("Addr   Hex   Known Device");
  Serial.println("--------------------------");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      const char* name = i2cDeviceName(addr);
      if (name) {
        Serial.printf("0x%02X   %02Xh   %s\n", addr, addr, name);
      } else {
        Serial.printf("0x%02X   %02Xh   [unknown device]\n", addr, addr);
      }
    }
  }
  Serial.println("--------------------------\n");
}

static void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(BMP280_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(BMP280_ADDR);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)BMP280_ADDR, (uint8_t)1);
  return Wire.read();
}

static uint16_t readRegister16(uint8_t reg) {
  Wire.beginTransmission(BMP280_ADDR);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)BMP280_ADDR, (uint8_t)2);
  uint8_t low = Wire.read();
  uint8_t high = Wire.read();
  return (uint16_t)high << 8 | low;
}

static int16_t readRegister16S(uint8_t reg) {
  return (int16_t)readRegister16(reg);
}

static void readCompensationParams() {
  dig_T1 = readRegister16(0x88);
  dig_T2 = readRegister16S(0x8A);
  dig_T3 = readRegister16S(0x8C);
  dig_P1 = readRegister16(0x8E);
  dig_P2 = readRegister16S(0x90);
  dig_P3 = readRegister16S(0x92);
  dig_P4 = readRegister16S(0x94);
  dig_P5 = readRegister16S(0x96);
  dig_P6 = readRegister16S(0x98);
  dig_P7 = readRegister16S(0x9A);
  dig_P8 = readRegister16S(0x9C);
  dig_P9 = readRegister16S(0x9E);
  // BME280 humidity compensation parameters (BMP280 ignores these)
  dig_H1 = (int16_t)readRegister(0xA1);  // unsigned
  dig_H2 = readRegister16S(0xE1);
  dig_H3 = readRegister(0xE3);
  // dig_H4 and dig_H5 are split across registers
  int16_t dig_H4_tmp = readRegister16S(0xE4);
  int16_t dig_H5_tmp = readRegister16S(0xE6);
  dig_H4 = (int16_t)((dig_H4_tmp & 0x0FFF) | ((readRegister(0xE5) & 0x0F) << 12));
  dig_H5 = (int16_t)((dig_H5_tmp >> 4) | ((readRegister(0xE5) & 0xF0) << 8));
  dig_H6 = (int16_t)(int8_t)readRegister(0xE7);
}

void initWeatherSensor() {
  Wire.begin();
  Wire.setClock(100000);

  // Scan I2C bus first
  scanI2C();

  // Check ID - BMP280 is 0x58, BME280 is 0x60
  // Use direct Wire calls to avoid address ambiguity
  Wire.beginTransmission(BMP280_ADDR);
  Wire.write(BMP280_REG_ID);
  uint8_t id = 0;
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom(BMP280_ADDR, (uint8_t)1);
    if (Wire.available()) id = Wire.read();
  }

  if (id != 0x58 && id != 0x60) {
    Serial.printf("BMP280/BME280: not found (ID=0x%02X, expected 0x58 or 0x60)\n", id);
    sensorFound = false;
    return;
  }

  Serial.printf("Environmental sensor found (ID=0x%02X)\n", id);
  isBME280 = (id == 0x60);

  // Read compensation parameters
  readCompensationParams();

  // Configure: forced mode, 1x oversampling for temp/pressure
  writeRegister(BMP280_REG_CTRL_MEAS, (1 << 5) | (1 << 2) | 0x01);
  writeRegister(BMP280_REG_CONFIG, (5 << 5));
  // BME280 needs ctrl_hum set for humidity readings
  if (isBME280) {
    writeRegister(BMP280_REG_CTRL_HUM, 0x01);  // 1x oversampling
  }

  sensorFound = true;
  Serial.println("Environmental sensor initialized");
}

static int32_t compensateTemperature(int32_t adc_T) {
  int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
  int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) >> 1) * ((adc_T >> 4) - ((int32_t)dig_T1)) >> 1) * ((int32_t)dig_T3)) >> 14;
  return ((var1 + var2 + 2) >> 2);
}

static uint32_t compensatePressure(int32_t adc_P, int32_t t_fine) {
  int64_t var1 = ((int64_t)t_fine) - 128000;
  int64_t var2 = var1 * var1 * (int64_t)dig_P6;
  var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
  var2 = var2 + (((int64_t)dig_P4) << 35);
  var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
  var1 = (((int64_t)1 << 47) + var1) * ((int64_t)dig_P1) >> 33;
  if (var1 == 0) return 0;
  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)dig_P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
  return (uint32_t)p;
}

// BME280 humidity compensation (BMP280 returns 0)
static int32_t compensateHumidity(int32_t adc_H, int32_t t_fine) {
  if (!isBME280) return 0;
  int32_t var1 = (int32_t)t_fine - 76800;
  int32_t var2 = (int32_t)((adc_H * 4) << 14);
  var2 = (var2 + ((int32_t)dig_H4 << 20)) + ((int32_t)dig_H5 * var1);
  int32_t var3 = (int32_t)dig_H2 * var1;
  int32_t var4 = (var3 >> 12);
  int32_t var5 = (((var2 - var4) >> 10) * ((int32_t)dig_H6)) >> 11;
  int32_t var6 = (((var2 >> 11) - 3) * ((int32_t)dig_H3)) >> 12;
  int32_t var7 = ((var5 + var6) >> 10) + 32768;
  int32_t compensated = (var7 * ((int32_t)dig_H1)) >> 12;
  compensated = (int32_t)(compensated + 2) >> 2;
  return (int32_t)(((compensated - 128) * 100) >> 10);  // percent * 1024
}

void readWeatherSensor(BME280Data* data) {
  if (!sensorFound) {
    data->valid = false;
    return;
  }

  // Trigger a reading in forced mode
  writeRegister(BMP280_REG_CTRL_MEAS, (1 << 5) | (1 << 2) | 0x01);
  delay(10);

  // Read pressure and temperature (8 bytes total)
  Wire.beginTransmission(BMP280_ADDR);
  Wire.write(BMP280_REG_DATA);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)BMP280_ADDR, (uint8_t)8);

  uint8_t dataPress_msb = Wire.read();
  uint8_t dataPress_lsb = Wire.read();
  uint8_t dataPress_xlsb = Wire.read();
  uint8_t dataTemp_msb = Wire.read();
  uint8_t dataTemp_lsb = Wire.read();
  uint8_t dataTemp_xlsb = Wire.read();
  uint8_t dataHum_msb = Wire.read();   // BME280 humidity MSB
  uint8_t dataHum_lsb = Wire.read();    // BME280 humidity LSB

  int32_t adc_P = ((int32_t)dataPress_msb << 12) | ((int32_t)dataPress_lsb << 4) | ((int32_t)(dataPress_xlsb >> 4) & 0x0F);
  int32_t adc_T = ((int32_t)dataTemp_msb << 12) | ((int32_t)dataTemp_lsb << 4) | ((int32_t)(dataTemp_xlsb >> 4) & 0x0F);
  int32_t adc_H = ((int32_t)dataHum_msb << 8) | dataHum_lsb;

  int32_t t_fine = compensateTemperature(adc_T);
  data->temperature = (float)(t_fine / 512.0f);
  data->pressure = (float)compensatePressure(adc_P, t_fine) / 256.0f;
  data->humidity = isBME280 ? ((float)compensateHumidity(adc_H, t_fine) / 1024.0f) : 0.0f;
  data->valid = true;

  // Also update global localWeather for macro access
  localWeather = *data;
}