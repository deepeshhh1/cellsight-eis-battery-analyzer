// ============================================================
//  ad5933.h - AD5933 IMPEDANCE CONVERTER: LOW-LEVEL DRIVER
//  Part of the CellSight EIS Battery Health Analyzer (v2).
// ------------------------------------------------------------
//  This header is the complete register-level driver for the
//  Analog Devices AD5933, the chip at the heart of CellSight.
//  It knows nothing about batteries, sweeps or SOH grading -
//  it only moves bytes over I2C and converts the chip's raw
//  DFT output into (magnitude, phase) pairs. All measurement
//  policy lives one level up, in eis.h.
//
//  HOW THE AD5933 MEASURES IMPEDANCE
//    The chip generates a sine excitation at a programmed
//    frequency, digitises the response current through the
//    unknown impedance, and runs a 1024-point DFT at exactly
//    that frequency. The DFT result lands in two signed
//    16-bit registers (REAL / IMAG). From them:
//
//        magnitude = sqrt(REAL^2 + IMAG^2)
//        phase     = atan2(IMAG, REAL)
//
//    The magnitude is NOT the impedance itself - it is the
//    response of the whole analogue chain (AFE gain, PGA,
//    ADC scaling). eis.h turns it into ohms using the gain
//    factor found during calibration:
//
//        |Z| = 1 / (gainFactor * magnitude)
//
//  PROGRAMMING MODEL USED BY THIS DRIVER
//    Every measurement follows the datasheet sequence:
//      reset -> program start freq / increments / settling ->
//      standby -> initialise-with-start-frequency -> start ->
//      poll STATUS until DATA_VALID -> read REAL / IMAG.
//    The REPEAT FREQUENCY command re-runs the DFT at the same
//    frequency without re-programming anything; eis.h uses it
//    for its per-point averaging.
//
//  The driver functions are implemented in this header (not a
//  separate .cpp) so the whole project stays a single Arduino
//  sketch folder: CellSight_EIS.ino + ad5933.h + eis.h.
//
//  Author : Deepesh Chauhan, B.Tech Electrical Engineering,
//           RGGEC Kangra
// ============================================================

#ifndef CELLSIGHT_AD5933_H
#define CELLSIGHT_AD5933_H

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ------------------------------------------------------------
//  DEVICE ADDRESS AND REGISTER MAP
//  The AD5933 sits at the fixed I2C address 0x0D. Registers are
//  reached by sending a pointer byte (0xB0 | address scheme,
//  see adRead) followed by data.
// ------------------------------------------------------------
#define AD5933_ADDR        0x0D   // fixed I2C slave address

#define REG_CTRL_H         0x80   // control register, high byte
#define REG_CTRL_L         0x81   // control register, low byte
#define REG_FREQ_START_H   0x82   // start frequency,  3 bytes (0x82-0x84)
#define REG_FREQ_INC_H     0x85   // frequency step, 3 bytes (0x85-0x87)
#define REG_NUM_INC_H      0x88   // number of increments, 2 bytes (0x88-0x89)
#define REG_SETTLE_H       0x8A   // settling-time cycles, 2 bytes (0x8A-0x8B)
#define REG_STATUS         0x8F   // status register
#define REG_TEMP_H         0x92   // on-chip temperature, 2 bytes (0x92-0x93)
#define REG_REAL_H         0x94   // DFT real component, signed 16-bit
#define REG_IMAG_H         0x96   // DFT imaginary component, signed 16-bit

// ------------------------------------------------------------
//  CONTROL REGISTER COMMANDS (upper nibble of REG_CTRL_H)
// ------------------------------------------------------------
#define CTRL_INIT          0x10   // initialise with start frequency
#define CTRL_START_SWEEP   0x20   // start frequency sweep
#define CTRL_INC_FREQ      0x30   // step to next frequency increment
#define CTRL_REPEAT_FREQ   0x40   // repeat the DFT at the same frequency
#define CTRL_MEAS_TEMP     0x90   // measure on-chip temperature
#define CTRL_POWER_DOWN    0xA0   // power-down mode
#define CTRL_STANDBY       0xB0   // standby mode (excitation off, clocks on)
#define CTRL_RESET         0x10   // reset bit, lives in REG_CTRL_L bit 4

// ------------------------------------------------------------
//  STATUS REGISTER BITS (REG_STATUS)
// ------------------------------------------------------------
#define STATUS_TEMP_VALID  0x01   // temperature data is valid
#define STATUS_DATA_VALID  0x02   // REAL/IMAG hold a finished DFT result
#define STATUS_SWEEP_DONE  0x04   // all programmed increments are done

// Internal master clock of the AD5933 (Hz). The frequency
// registers are programmed in units of (MCLK / 4) / 2^27.
#define AD5933_MCLK_HZ     16776000.0

// ------------------------------------------------------------
//  adWrite - write one byte to one register.
// ------------------------------------------------------------
inline void adWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// ------------------------------------------------------------
//  adRead - read one byte from one register.
//  The AD5933 has no auto-incrementing read pointer, so the
//  register address is first loaded with the "set pointer"
//  command byte (0xB0) and the byte is then clocked out.
// ------------------------------------------------------------
inline uint8_t adRead(uint8_t reg) {
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(0xB0);                 // set-pointer command
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(AD5933_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// ------------------------------------------------------------
//  adReadWord - read a signed 16-bit big-endian register pair
//  (used for the REAL and IMAG DFT results and temperature).
// ------------------------------------------------------------
inline int16_t adReadWord(uint8_t regH) {
  uint8_t hi = adRead(regH);
  uint8_t lo = adRead(regH + 1);
  return (int16_t)((hi << 8) | lo);
}

// ------------------------------------------------------------
//  adSetFrequency - program a 24-bit frequency register.
//  regBase is REG_FREQ_START_H or REG_FREQ_INC_H. The coding
//  follows the datasheet:
//
//      code = (freqHz / (MCLK / 4)) * 2^27
//
//  With the internal 16.776 MHz clock this resolves frequency
//  to well under 0.01 Hz across the CellSight sweep range.
// ------------------------------------------------------------
inline void adSetFrequency(uint8_t regBase, float freqHz) {
  uint32_t code = (uint32_t)((freqHz / (AD5933_MCLK_HZ / 4.0)) * 134217728.0);
  adWrite(regBase,     (code >> 16) & 0xFF);
  adWrite(regBase + 1, (code >> 8)  & 0xFF);
  adWrite(regBase + 2,  code        & 0xFF);
}

// ------------------------------------------------------------
//  adControl - issue a control command (CTRL_* above).
//  The low nibble of CTRL_H carries the output-range and PGA
//  settings; CellSight fixes them at range 1 (2 V p-p nominal
//  excitation before the AFE) with PGA gain x1. The low byte
//  of the control register is preserved across the command.
// ------------------------------------------------------------
inline void adControl(uint8_t cmd) {
  uint8_t l = adRead(REG_CTRL_L);
  adWrite(REG_CTRL_H, cmd | 0x01);        // range 1, PGA x1
  adWrite(REG_CTRL_L, l);
}

// ------------------------------------------------------------
//  adReset - return the chip to a known state. The reset bit
//  is bit 4 of the LOW control byte (unlike the commands,
//  which live in the high byte).
// ------------------------------------------------------------
inline void adReset() {
  adWrite(REG_CTRL_L, CTRL_RESET);
}

// ------------------------------------------------------------
//  adMeasure - wait for one DFT conversion and read it back.
//
//  Polls STATUS until DATA_VALID (2 s timeout, returns false)
//  and converts the REAL/IMAG pair into magnitude and phase
//  in degrees. The phase is the RAW system phase of the whole
//  measurement chain; eis.h subtracts the calibration phase
//  offset before interpreting it as the cell's phase.
//
//  Note: adMeasure() deliberately does not distinguish the
//  first conversion at a frequency from a REPEAT FREQUENCY
//  conversion. Callers arm the frequency (INIT + START), call
//  adMeasure() once, then issue adControl(CTRL_REPEAT_FREQ)
//  and call adMeasure() again for each repeat.
// ------------------------------------------------------------
inline bool adMeasure(float& magnitude, float& phaseDeg) {
  unsigned long t0 = millis();
  while (!(adRead(REG_STATUS) & STATUS_DATA_VALID)) {
    if (millis() - t0 > 2000) return false;   // conversion timeout
    delay(2);
  }
  int16_t re = adReadWord(REG_REAL_H);
  int16_t im = adReadWord(REG_IMAG_H);
  magnitude = sqrt((double)re * re + (double)im * im);
  phaseDeg  = atan2((double)im, (double)re) * 180.0 / PI;
  return true;
}

// ------------------------------------------------------------
//  adReadTemperature - read the AD5933's on-chip temperature
//  sensor. This measures the DIE temperature of the converter
//  itself, not the cell - CellSight takes the cell temperature
//  from its DS18B20 (see the main sketch). The die reading is
//  still useful as a diagnostic: a converter running far above
//  ambient is a sign the AFE is dissipating more than it
//  should, which would drift the gain factor between
//  calibration and measurement.
//
//  Decoding follows the datasheet: the 14-bit result is in
//  REG_TEMP_H/REG_TEMP_L, bit 13 is the sign, and the scale is
//  32 codes per degree C:
//      positive:  T = code / 32
//      negative:  T = (code - 16384) / 32
//  Accuracy is only +/-2 degC - a health indicator, never a
//  measurement input. Returns false on timeout.
// ------------------------------------------------------------
inline bool adReadTemperature(float& tempC) {
  adControl(CTRL_MEAS_TEMP);
  unsigned long t0 = millis();
  while (!(adRead(REG_STATUS) & STATUS_TEMP_VALID)) {
    if (millis() - t0 > 500) return false;    // conversion timeout
    delay(2);
  }
  int16_t raw = adReadWord(REG_TEMP_H) & 0x3FFF;
  if (raw & 0x2000) tempC = (raw - 16384) / 32.0f;   // negative value
  else              tempC = raw / 32.0f;
  return true;
}

// ------------------------------------------------------------
//  adSelfTest - is a live AD5933 actually on the bus?
//  The chip has no ID register to interrogate, so presence is
//  proven by a write/readback round trip: a test pattern is
//  written into the settling-time register pair (inert while
//  the part is powered down) and read back. Both bytes must
//  match. This distinguishes "chip absent / bus fault" from
//  "chip fine, cell or AFE at fault" at boot, which saves a
//  long and misleading debugging session later.
//  The settling register is not restored afterwards: both
//  the calibration and the sweep sequences in eis.h program
//  it fresh before every measurement run.
// ------------------------------------------------------------
inline bool adSelfTest() {
  adControl(CTRL_POWER_DOWN);
  adWrite(REG_SETTLE_H,     0x2A);
  adWrite(REG_SETTLE_H + 1, 0x15);
  bool ok = (adRead(REG_SETTLE_H)     == 0x2A) &&
            (adRead(REG_SETTLE_H + 1) == 0x15);
  adWrite(REG_SETTLE_H,     0x00);
  adWrite(REG_SETTLE_H + 1, 0x00);
  return ok;
}

#endif  // CELLSIGHT_AD5933_H
