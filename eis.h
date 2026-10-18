// ============================================================
//  eis.h - EIS MEASUREMENT ENGINE: CALIBRATION, SWEEP, ANALYSIS
//  Part of the CellSight EIS Battery Health Analyzer (v2).
// ------------------------------------------------------------
//  This module owns everything between "the AD5933 can run a
//  DFT" (ad5933.h) and "the dashboard can show a grade":
//
//    runCalibration()  - measure a known precision resistor
//                        and derive the system gain factor
//                        and phase offset.
//    runSweep()        - walk the log-spaced frequency table,
//                        convert DFT output to complex ohms,
//                        correct for cell temperature, and
//                        store the spectrum.
//    analyzeSpectrum() - extract R_ohmic, R_ct and the Warburg
//                        slope from the Nyquist curve and grade
//                        the cell's State of Health (SOH).
//
//  v2 MEASUREMENT IMPROVEMENTS (over the v1 single-file build)
//    1. PER-POINT AVERAGING. Every frequency point is converted
//       POINT_AVERAGES (3) times - first conversion plus two
//       REPEAT FREQUENCY conversions - and averaged. Magnitude
//       uses the arithmetic mean; phase uses the CIRCULAR mean
//       (mean of unit vectors), which stays correct if samples
//       straddle the +/-180 degree wrap, where a plain average
//       of angles would collapse toward zero.
//    2. TEMPERATURE COMPENSATION. Cell impedance is strongly
//       temperature dependent, so a cell measured cold grades
//       worse than the same cell warm. Each sweep is corrected
//       back to the 25 degC reference using the DS18B20 cell
//       temperature (see tempCorrectionFactor()).
//
//  SERVICES PROVIDED BY THE MAIN SKETCH
//    This module is deliberately free of Wi-Fi/web objects.
//    The sketch supplies three things it declares extern here:
//      cellTempC       - latest DS18B20 reading (sketch-owned)
//      logEvent()      - event log sink (dashboard + serial)
//      serviceClients()- web-server pump, called inside the
//                        sweep loop so the UI never freezes
//                        during a long low-frequency sweep.
//
//  Author : Deepesh Chauhan, B.Tech Electrical Engineering,
//           RGGEC Kangra
// ============================================================

#ifndef CELLSIGHT_EIS_H
#define CELLSIGHT_EIS_H

#include <Arduino.h>
#include <math.h>
#include "ad5933.h"

// ------------------------------------------------------------
//  SERVICES OWNED BY THE MAIN SKETCH (defined in the .ino)
// ------------------------------------------------------------
extern float cellTempC;                 // DS18B20 cell temperature
void logEvent(const String& msg);       // event log (dashboard + Serial)
void serviceClients();                  // web-server pump for long loops

// ------------------------------------------------------------
//  SWEEP CONFIGURATION
//  The sweep runs from high frequency down to low frequency:
//  the high-frequency points settle fast and pin down R_ohmic
//  before the slow low-frequency tail is measured.
// ------------------------------------------------------------
const float SWEEP_START_HZ = 10000.0f;   // first (highest) frequency
const float SWEEP_END_HZ   = 10.0f;      // last (lowest) frequency
const int   SWEEP_POINTS   = 40;         // log-spaced points per sweep
const int   SETTLE_CYCLES  = 15;         // settling cycles per point

// v2: conversions averaged per frequency point (1 initial +
// 2 REPEAT FREQUENCY conversions). Averaging three independent
// DFTs cuts random noise by roughly sqrt(3) and exposes any
// point where the cell or the AFE misbehaved mid-sweep.
const int   POINT_AVERAGES = 3;

// ------------------------------------------------------------
//  CALIBRATION
//  CAL_RES_OHMS is the precision resistor fitted in place of
//  the cell during calibration. The AFE's gain and every other
//  chain constant are folded into the gain factor derived
//  from it, using |Z| = 1 / (gainFactor * DFT magnitude).
// ------------------------------------------------------------
const float CAL_RES_OHMS   = 10.0f;

// ------------------------------------------------------------
//  FRESH-CELL BASELINE (SOH reference)
//  Measured once on a known-new cell through the same AFE and
//  then stored here. The defaults below suit a typical 18650
//  Li-ion cell on the reference AFE - replace them with your
//  own baseline before trusting absolute grades.
// ------------------------------------------------------------
float baseline_R_ohmic = 0.045f;         // ohms
float baseline_R_ct    = 0.060f;         // ohms

// ------------------------------------------------------------
//  TEMPERATURE COMPENSATION
//  Li-ion impedance falls as the cell warms. Near room
//  temperature the standard first-order approximation used
//  across the EIS literature is a linear coefficient of about
//  -2 %/degC on the impedance magnitude:
//
//      Z(T) ~= Z(25C) * (1 - 0.02 * (T - 25))
//
//  This is a FIRST-ORDER approximation only: it is reasonable
//  roughly between 10 and 40 degC, it applies one coefficient
//  to the whole spectrum although R_ct in reality moves more
//  than R_ohmic, and it is no substitute for measuring at a
//  controlled temperature when grades really matter. It exists
//  so that a cell measured on a cold morning is not unfairly
//  graded against a baseline taken in a warm lab.
// ------------------------------------------------------------
const float TEMP_REF_C       = 25.0f;    // reference temperature
const float TEMP_COEFF_PER_C = -0.02f;   // -2 %/degC on |Z|

// Multiplicative factor that maps an impedance measured at
// tempC back to the TEMP_REF_C reference. Phase is a ratio of
// components and is left uncorrected.
inline float tempCorrectionFactor(float tempC) {
  float denom = 1.0f + TEMP_COEFF_PER_C * (tempC - TEMP_REF_C);
  if (denom < 0.5f) denom = 0.5f;        // sanity clamp, extreme cold
  return 1.0f / denom;
}

// ------------------------------------------------------------
//  SPECTRUM STORAGE
//  One EisPoint per frequency. zReal/zImag/zMag are stored
//  AFTER temperature compensation, i.e. they are always
//  25 degC-referenced values; sweepTempC records the cell
//  temperature the correction was based on.
// ------------------------------------------------------------
struct EisPoint {
  float freq;        // Hz
  float zReal;       // ohms (Z'), temperature-compensated
  float zImag;       // ohms (Z'', negative for capacitive)
  float zMag;        // ohms (|Z|), temperature-compensated
  float phaseDeg;    // degrees, calibration phase removed
  float spreadPct;   // repeatability of the point: (max - min) /
                     // mean of the averaged DFT magnitudes, in %.
                     // A quiet spectrum sits well under 1%; a
                     // point far above that was disturbed (loose
                     // lead, cell gassing, AFE overload) and its
                     // neighbours should be read with suspicion.
};

EisPoint sweep[SWEEP_POINTS];
int   sweepCount       = 0;              // valid points in sweep[]
bool  sweepRunning     = false;
bool  sweepDone        = false;
float sweepTempC       = TEMP_REF_C;     // cell temp during last sweep
unsigned long sweepTimestampMs = 0;      // millis() at sweep completion

double gainFactor  = 0.0;                // from calibration (0 = none)
float  systemPhase = 0.0f;               // chain phase offset, degrees

// Analysis outputs (filled by analyzeSpectrum()).
float rOhmic = 0, rCt = 0, warburgSlope = 0;
float sohPercent = 0;
String sohGrade = "NOT MEASURED";

// Forward declaration: runSweep() invokes the analysis on
// completion; the definition sits at the end of this header
// so the file reads in measurement order.
void analyzeSpectrum();

// ------------------------------------------------------------
//  measureAveraged - one frequency point, averaged.
//  The caller has already armed the current frequency (INIT +
//  START SWEEP). The first conversion is read directly; the
//  remaining POINT_AVERAGES - 1 conversions are requested with
//  CTRL_REPEAT_FREQ, which re-runs the DFT in place.
//  Magnitude is averaged arithmetically, phase by circular
//  mean (see the module header). spreadPct reports how far
//  apart the individual magnitudes were, as a percentage of
//  their mean - the point's repeatability figure.
//  Returns false on conversion timeout or a zero magnitude
//  (a zero DFT magnitude means the chain saw no signal at
//  all - open lead or a tripped AFE - and must not be
//  averaged into a spectrum as if it were data).
// ------------------------------------------------------------
static bool measureAveraged(float& avgMag, float& avgPhaseDeg,
                            float& spreadPct) {
  double magSum = 0.0;
  double sinSum = 0.0, cosSum = 0.0;      // for the circular phase mean
  float magMin = 0, magMax = 0;
  for (int k = 0; k < POINT_AVERAGES; k++) {
    if (k > 0) adControl(CTRL_REPEAT_FREQ);
    float mag, ph;
    if (!adMeasure(mag, ph) || mag == 0) return false;
    magSum += mag;
    if (k == 0 || mag < magMin) magMin = mag;
    if (k == 0 || mag > magMax) magMax = mag;
    double rad = (double)ph * PI / 180.0;
    sinSum += sin(rad);
    cosSum += cos(rad);
  }
  avgMag = (float)(magSum / POINT_AVERAGES);
  avgPhaseDeg = (float)(atan2(sinSum, cosSum) * 180.0 / PI);
  spreadPct = (avgMag > 0)
      ? (magMax - magMin) / avgMag * 100.0f : 0.0f;
  return true;
}

// ------------------------------------------------------------
//  runCalibration - derive gain factor and system phase.
//  Fit the CAL_RES precision resistor in place of the cell,
//  then call this (dashboard 'Calibrate', serial 'c', or the
//  boot-time auto-calibration in the sketch). A single spot
//  frequency of 1 kHz is used: high enough that the resistor's
//  parasitics are negligible, low enough to be mid-band for
//  the AFE. The resistor itself is treated as temperature-
//  stable (a metal-film part drifts ~50 ppm/degC, three
//  orders below the cell effects corrected in runSweep()).
// ------------------------------------------------------------
bool runCalibration() {
  adReset();
  delay(5);
  adSetFrequency(REG_FREQ_START_H, 1000.0f);
  adSetFrequency(REG_FREQ_INC_H, 0.0f);
  adWrite(REG_NUM_INC_H, 0); adWrite(REG_NUM_INC_H + 1, 0);
  adWrite(REG_SETTLE_H, 0);  adWrite(REG_SETTLE_H + 1, SETTLE_CYCLES);
  adControl(CTRL_STANDBY);
  adControl(CTRL_INIT);
  delay(10);
  adControl(CTRL_START_SWEEP);
  float mag, ph, spread;
  if (!measureAveraged(mag, ph, spread)) {
    adControl(CTRL_POWER_DOWN);
    return false;
  }
  gainFactor  = 1.0 / (CAL_RES_OHMS * mag);
  systemPhase = ph;
  adControl(CTRL_POWER_DOWN);
  logEvent("Calibration OK. GF=" + String(gainFactor, 6) +
           " (repeatability " + String(spread, 2) + "%)");
  return true;
}

// ------------------------------------------------------------
//  runSweep - full log-spaced EIS sweep, high to low frequency.
//  For every point the chip is fully re-programmed (reset,
//  start frequency, no increments) rather than using the
//  on-chip increment engine: each point then gets the full
//  settling time and the REPEAT-based averaging above, and a
//  disturbed point can simply be skipped instead of poisoning
//  an on-chip sequence. Points that fail to convert are
//  skipped; the sweep is usable as long as analyzeSpectrum()
//  still finds enough points.
// ------------------------------------------------------------
void runSweep() {
  sweepRunning = true;
  sweepDone = false;
  sweepCount = 0;

  // The temperature correction for the whole sweep is based on
  // the cell temperature at sweep start (a sweep takes seconds
  // to a couple of minutes; the cell will not change temperature
  // measurably in that time at EIS perturbation levels).
  sweepTempC = cellTempC;
  const float tCorr = tempCorrectionFactor(sweepTempC);
  double spreadSum = 0.0;               // repeatability bookkeeping

  // Log-spaced frequency table, SWEEP_START_HZ -> SWEEP_END_HZ.
  float freqs[SWEEP_POINTS];
  float ratio = pow(SWEEP_END_HZ / SWEEP_START_HZ, 1.0f / (SWEEP_POINTS - 1));
  for (int i = 0; i < SWEEP_POINTS; i++)
    freqs[i] = SWEEP_START_HZ * pow(ratio, i);

  for (int i = 0; i < SWEEP_POINTS; i++) {
    adReset();
    delay(2);
    adSetFrequency(REG_FREQ_START_H, freqs[i]);
    adSetFrequency(REG_FREQ_INC_H, 0.0f);
    adWrite(REG_NUM_INC_H, 0); adWrite(REG_NUM_INC_H + 1, 0);
    adWrite(REG_SETTLE_H, 0);  adWrite(REG_SETTLE_H + 1, SETTLE_CYCLES);
    adControl(CTRL_STANDBY);
    adControl(CTRL_INIT);
    delay(5);
    adControl(CTRL_START_SWEEP);

    float mag, ph, spread;
    if (!measureAveraged(mag, ph, spread)) continue;   // skip a failed point

    // DFT magnitude -> ohms via the calibration gain factor,
    // then temperature-compensate the magnitude back to the
    // 25 degC reference. The compensated magnitude and the
    // (chain-corrected) phase rebuild the complex impedance.
    double zMag = tCorr / (gainFactor * mag);
    double zPhaseRad = (ph - systemPhase) * PI / 180.0;

    EisPoint& p = sweep[sweepCount];
    p.freq     = freqs[i];
    p.zMag     = (float)zMag;
    p.phaseDeg = ph - systemPhase;
    p.zReal    = (float)(zMag * cos(zPhaseRad));
    p.zImag    = (float)(zMag * sin(zPhaseRad));
    p.spreadPct = spread;
    sweepCount++;
    spreadSum += spread;

    serviceClients();                 // keep the web UI responsive
    yield();
  }
  adControl(CTRL_POWER_DOWN);
  sweepRunning = false;
  sweepDone = true;
  sweepTimestampMs = millis();
  analyzeSpectrum();
  float meanSpread = (sweepCount > 0) ? spreadSum / sweepCount : 0;
  logEvent("EIS sweep complete: " + String(sweepCount) + " points @ " +
           String(sweepTempC, 1) + "C (corrected to 25C), mean spread " +
           String(meanSpread, 2) + "%");
}

// ------------------------------------------------------------
//  analyzeSpectrum - Nyquist feature extraction + SOH grade.
//    R_ohmic : Z' at the highest measured frequencies (the
//              left-hand intercept of the Nyquist curve).
//    R_ct    : diameter of the mid-frequency semicircle,
//              estimated from the -Z'' peak and its low-side
//              half-height crossing, minus R_ohmic.
//    Warburg : slope of the low-frequency tail in Z' vs
//              1/sqrt(f) coordinates (diffusion behaviour).
//  SOH grading: ohmic growth dominates end-of-life - a cell
//  whose R_ohmic has doubled sits near 80% SOH in most grading
//  schemes. CellSight combines ohmic growth (60%) and R_ct
//  growth (40%) against the stored fresh-cell baseline.
//  Fewer than 8 valid points cannot support any of this, so
//  the previous results are left untouched in that case.
// ------------------------------------------------------------
void analyzeSpectrum() {
  if (sweepCount < 8) return;

  // R_ohmic: average Z' of the 3 highest-frequency points
  float sum = 0;
  for (int i = 0; i < 3 && i < sweepCount; i++) sum += sweep[i].zReal;
  rOhmic = sum / min(3, sweepCount);

  // Semicircle peak: maximum -Z'' in the mid band
  int peakIdx = -1; float peakVal = 0;
  for (int i = 0; i < sweepCount; i++) {
    float neg = -sweep[i].zImag;
    if (sweep[i].freq < 5000 && sweep[i].freq > 5 && neg > peakVal) {
      peakVal = neg; peakIdx = i;
    }
  }
  if (peakIdx > 0) {
    // Diameter approx: Z' where -Z'' falls back near the peak's
    // half height on the low-frequency side, minus R_ohmic.
    float lowCross = sweep[peakIdx].zReal;
    for (int i = peakIdx; i < sweepCount; i++) {
      if (-sweep[i].zImag < peakVal * 0.5f) { lowCross = sweep[i].zReal; break; }
      lowCross = sweep[i].zReal;
    }
    rCt = max(0.0f, lowCross - rOhmic);
  }

  // Warburg slope: fit Z' against 1/sqrt(f) on the 5 lowest points
  int n = min(5, sweepCount);
  float sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = sweepCount - n; i < sweepCount; i++) {
    float x = 1.0f / sqrt(sweep[i].freq);
    float y = sweep[i].zReal;
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  float denom = n * sxx - sx * sx;
  warburgSlope = (denom != 0) ? (n * sxy - sx * sy) / denom : 0;

  // ---- SOH grading -------------------------------------------
  float ohmicScore = 100.0f - 50.0f * max(0.0f, (rOhmic / baseline_R_ohmic) - 1.0f);
  float ctScore    = 100.0f - 40.0f * max(0.0f, (rCt    / baseline_R_ct)    - 1.0f);
  sohPercent = constrain(0.6f * ohmicScore + 0.4f * ctScore, 0.0f, 100.0f);

  if      (sohPercent >= 90) sohGrade = "A - EXCELLENT (EV duty)";
  else if (sohPercent >= 80) sohGrade = "B - GOOD (EV duty)";
  else if (sohPercent >= 70) sohGrade = "C - SECOND LIFE (storage)";
  else if (sohPercent >= 60) sohGrade = "D - SECOND LIFE (light duty)";
  else                       sohGrade = "E - RECYCLE";

  logEvent("R_ohmic=" + String(rOhmic * 1000, 1) + "mOhm R_ct=" +
           String(rCt * 1000, 1) + "mOhm Warburg=" +
           String(warburgSlope * 1000, 1) + "mOhm/sqrt(Hz) SOH=" +
           String(sohPercent, 0) + "% Grade " + sohGrade);
}

#endif  // CELLSIGHT_EIS_H
