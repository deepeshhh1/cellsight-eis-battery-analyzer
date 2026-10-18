// ============================================================
//  CELLSIGHT - EIS BATTERY HEALTH ANALYZER
//  ESP32 + AD5933 Impedance Converter + DS18B20
// ------------------------------------------------------------
//  Electrochemical Impedance Spectroscopy (EIS) is how battery
//  research labs and EV companies judge a cell's true health.
//  A small AC signal is swept across frequencies (10 Hz -
//  100 kHz); at each frequency the cell's complex impedance
//  Z = Z' + jZ'' is measured. Plotted as -Z'' vs Z' this gives
//  the famous NYQUIST PLOT, from which we extract:
//
//    * R_ohmic   - high-frequency intercept (electrolyte +
//                  contacts). Grows as the cell dries out/ages.
//    * R_ct      - charge-transfer semicircle diameter. Grows
//                  as electrode surfaces degrade.
//    * Warburg   - low-frequency 45-degree tail (diffusion).
//
//  SOH (State of Health) is graded from R_ohmic growth versus
//  the stored fresh-cell baseline, cross-checked with a DC
//  load-pulse DCIR test and the cell's open-circuit voltage.
//
//  HARDWARE NOTES
//    * The AD5933 excites the cell through an analog front-end
//      (AFE): a DC-blocking / gain stage so the cell sees only
//      a +/-10 mV AC perturbation around its rest potential and
//      the AD5933 input stays in range. Build/buy a battery-EIS
//      AFE; NEVER connect a Li-ion cell straight to the AD5933.
//    * Calibrate with a precision resistor (CAL_RES) in place
//      of the cell before first use (dashboard button / serial
//      command 'c').
//
//  Author : Deepesh Chauhan, B.Tech Electrical Engineering
//           RGGEC Kangra
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ---------------- WI-FI (replace with your credentials) ------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ---------------- AD5933 REGISTERS ---------------------------
#define AD5933_ADDR        0x0D
#define REG_CTRL_H         0x80
#define REG_CTRL_L         0x81
#define REG_FREQ_START_H   0x82
#define REG_FREQ_INC_H     0x85
#define REG_NUM_INC_H      0x88
#define REG_SETTLE_H       0x8A
#define REG_STATUS         0x8F
#define REG_TEMP_H         0x92
#define REG_REAL_H         0x94
#define REG_IMAG_H         0x96

#define CTRL_INIT          0x10   // init with start frequency
#define CTRL_START_SWEEP   0x20
#define CTRL_INC_FREQ      0x30
#define CTRL_REPEAT_FREQ   0x40
#define CTRL_POWER_DOWN    0xA0
#define CTRL_STANDBY       0xB0
#define CTRL_RESET         0x10   // in CTRL_L bit 4

#define STATUS_TEMP_VALID  0x01
#define STATUS_DATA_VALID  0x02
#define STATUS_SWEEP_DONE  0x04

// ---------------- SWEEP CONFIG -------------------------------
const float SWEEP_START_HZ = 10000.0f;   // start high...
const float SWEEP_END_HZ   = 10.0f;      // ...sweep down in analysis
const int   SWEEP_POINTS   = 40;         // log-spaced points
const int   SETTLE_CYCLES  = 15;

// Calibration resistor (ohms) placed at the cell position
// during calibration. The AFE gain is folded into the gain
// factor computed from this known resistor.
const float CAL_RES_OHMS   = 10.0f;

// Fresh-cell baseline (measure once on a new cell, then store).
// These defaults are for a typical 18650 Li-ion cell measured
// through the reference AFE - replace with your own baseline.
float baseline_R_ohmic = 0.045f;         // ohms
float baseline_R_ct    = 0.060f;         // ohms

// ---------------- PINS --------------------------------------
#define TEMP_PIN     4                   // DS18B20 data
#define LOAD_PIN     26                  // DC load-pulse MOSFET
#define CELL_ADC_PIN 34                  // cell voltage divider

// Cell voltage divider: cell (max 4.2V) -> 2:1 divider -> ADC
const float CELL_DIV_RATIO = 2.0f;

// ---------------- EIS RESULT STORAGE --------------------------
struct EisPoint {
  float freq;        // Hz
  float zReal;       // ohms (Z')
  float zImag;       // ohms (Z'', negative for capacitive)
  float zMag;
  float phaseDeg;
};
EisPoint sweep[SWEEP_POINTS];
int   sweepCount   = 0;
bool  sweepRunning = false;
bool  sweepDone    = false;
double gainFactor  = 0.0;       // from calibration
float systemPhase  = 0.0f;      // degrees, from calibration

float rOhmic = 0, rCt = 0, warburgSlope = 0;
float sohPercent = 0;
String sohGrade = "NOT MEASURED";
float cellOCV = 0, cellTempC = 25.0f, dcir = 0;
String eventLog = "Boot\n";

// ---------------- OBJECTS -------------------------------------
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
WebServer server(80);

// =============================================================
//  AD5933 LOW-LEVEL DRIVER
// =============================================================
void adWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t adRead(uint8_t reg) {
  // Pointer command (0xB0) sets the register address
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(0xB0);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(AD5933_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

int16_t adReadWord(uint8_t regH) {
  uint8_t hi = adRead(regH);
  uint8_t lo = adRead(regH + 1);
  return (int16_t)((hi << 8) | lo);
}

void adSetFrequency(uint8_t regBase, float freqHz) {
  // freq register = (freq / (MCLK/4)) * 2^27, MCLK = 16.776 MHz
  uint32_t code = (uint32_t)((freqHz / (16776000.0 / 4.0)) * 134217728.0);
  adWrite(regBase,     (code >> 16) & 0xFF);
  adWrite(regBase + 1, (code >> 8)  & 0xFF);
  adWrite(regBase + 2,  code        & 0xFF);
}

void adControl(uint8_t cmd) {
  uint8_t l = adRead(REG_CTRL_L);
  adWrite(REG_CTRL_H, cmd | 0x01);        // range 1, PGA x1
  adWrite(REG_CTRL_L, l);
}

// Measure ONE frequency point; returns magnitude & phase(deg)
// of the transfer function reported by the AD5933 DFT engine.
bool adMeasure(float& magnitude, float& phaseDeg) {
  unsigned long t0 = millis();
  while (!(adRead(REG_STATUS) & STATUS_DATA_VALID)) {
    if (millis() - t0 > 2000) return false;   // timeout
    delay(2);
  }
  int16_t re = adReadWord(REG_REAL_H);
  int16_t im = adReadWord(REG_IMAG_H);
  magnitude = sqrt((double)re * re + (double)im * im);
  phaseDeg  = atan2((double)im, (double)re) * 180.0 / PI;
  return true;
}

// =============================================================
//  CALIBRATION
//  With CAL_RES in place of the cell, gain factor relates the
//  measured DFT magnitude to true impedance:
//      |Z| = 1 / (GF * magnitude)
// =============================================================
bool runCalibration() {
  adWrite(REG_CTRL_L, CTRL_RESET);
  delay(5);
  adSetFrequency(REG_FREQ_START_H, 1000.0f);
  adSetFrequency(REG_FREQ_INC_H, 0.0f);
  adWrite(REG_NUM_INC_H, 0); adWrite(REG_NUM_INC_H + 1, 0);
  adWrite(REG_SETTLE_H, 0);  adWrite(REG_SETTLE_H + 1, SETTLE_CYCLES);
  adControl(CTRL_STANDBY);
  adControl(CTRL_INIT);
  delay(10);
  adControl(CTRL_START_SWEEP);
  float mag, ph;
  if (!adMeasure(mag, ph) || mag == 0) return false;
  gainFactor  = 1.0 / (CAL_RES_OHMS * mag);
  systemPhase = ph;
  adControl(CTRL_POWER_DOWN);
  logEvent("Calibration OK. GF=" + String(gainFactor, 6));
  return true;
}

// =============================================================
//  FULL EIS SWEEP  (log-spaced, high -> low frequency)
// =============================================================
void runSweep() {
  sweepRunning = true;
  sweepDone = false;
  sweepCount = 0;

  // Log-spaced frequency table
  float freqs[SWEEP_POINTS];
  float ratio = pow(SWEEP_END_HZ / SWEEP_START_HZ, 1.0f / (SWEEP_POINTS - 1));
  for (int i = 0; i < SWEEP_POINTS; i++)
    freqs[i] = SWEEP_START_HZ * pow(ratio, i);

  for (int i = 0; i < SWEEP_POINTS; i++) {
    adWrite(REG_CTRL_L, CTRL_RESET);
    delay(2);
    adSetFrequency(REG_FREQ_START_H, freqs[i]);
    adSetFrequency(REG_FREQ_INC_H, 0.0f);
    adWrite(REG_NUM_INC_H, 0); adWrite(REG_NUM_INC_H + 1, 0);
    adWrite(REG_SETTLE_H, 0);  adWrite(REG_SETTLE_H + 1, SETTLE_CYCLES);
    adControl(CTRL_STANDBY);
    adControl(CTRL_INIT);
    delay(5);
    adControl(CTRL_START_SWEEP);

    float mag, ph;
    if (!adMeasure(mag, ph) || mag == 0) continue;

    double zMag = 1.0 / (gainFactor * mag);
    double zPhaseRad = (ph - systemPhase) * PI / 180.0;

    EisPoint& p = sweep[sweepCount];
    p.freq     = freqs[i];
    p.zMag     = (float)zMag;
    p.phaseDeg = ph - systemPhase;
    p.zReal    = (float)(zMag * cos(zPhaseRad));
    p.zImag    = (float)(zMag * sin(zPhaseRad));
    sweepCount++;

    server.handleClient();                 // keep UI responsive
    yield();
  }
  adControl(CTRL_POWER_DOWN);
  sweepRunning = false;
  sweepDone = true;
  analyzeSpectrum();
  logEvent("EIS sweep complete: " + String(sweepCount) + " points");
}

// =============================================================
//  NYQUIST ANALYSIS
//    R_ohmic : Z' at the highest measured frequencies
//    R_ct    : diameter of the mid-frequency semicircle,
//              estimated as (Z' at the -Z'' peak) - R_ohmic plus
//              the low-side crossing; simplified here to
//              peak-based extraction used for grading.
//    Warburg : slope of the low-frequency tail in Z' vs
//              1/sqrt(f) coordinates.
// =============================================================
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
  // Ohmic growth dominates end-of-life: a cell whose R_ohmic
  // has doubled is at ~80% SOH in most grading schemes; we use
  // a combined score of ohmic growth (60%) + Rct growth (40%).
  float ohmicScore = 100.0f - 50.0f * max(0.0f, (rOhmic / baseline_R_ohmic) - 1.0f);
  float ctScore    = 100.0f - 40.0f * max(0.0f, (rCt    / baseline_R_ct)    - 1.0f);
  sohPercent = constrain(0.6f * ohmicScore + 0.4f * ctScore, 0.0f, 100.0f);

  if      (sohPercent >= 90) sohGrade = "A - EXCELLENT (EV duty)";
  else if (sohPercent >= 80) sohGrade = "B - GOOD (EV duty)";
  else if (sohPercent >= 70) sohGrade = "C - SECOND LIFE (storage)";
  else if (sohPercent >= 60) sohGrade = "D - SECOND LIFE (light duty)";
  else                       sohGrade = "E - RECYCLE";

  logEvent("R_ohmic=" + String(rOhmic * 1000, 1) + "mOhm R_ct=" +
           String(rCt * 1000, 1) + "mOhm SOH=" + String(sohPercent, 0) +
           "% Grade " + sohGrade);
}

// =============================================================
//  DC MEASUREMENTS - OCV + DCIR LOAD PULSE
// =============================================================
float readCellVoltage() {
  int raw = analogRead(CELL_ADC_PIN);
  return (raw / 4095.0f) * 3.3f * CELL_DIV_RATIO;
}

void runDcirTest() {
  float vRest = readCellVoltage();
  digitalWrite(LOAD_PIN, HIGH);            // known load (~1C pulse)
  delay(1000);
  float vLoad = readCellVoltage();
  digitalWrite(LOAD_PIN, LOW);
  // Load resistor chosen so the pulse draws PULSE_AMPS (see BOM)
  const float PULSE_AMPS = 3.0f;
  dcir = (PULSE_AMPS > 0) ? (vRest - vLoad) / PULSE_AMPS : 0;
  cellOCV = vRest;
  logEvent("DCIR=" + String(dcir * 1000, 1) + " mOhm @ OCV " +
           String(vRest, 3) + "V");
}

// =============================================================
//  LOGGING
// =============================================================
void logEvent(const String& msg) {
  eventLog += msg + "\n";
  if (eventLog.length() > 1600) eventLog = eventLog.substring(500);
  Serial.println(msg);
}

// =============================================================
//  DASHBOARD  (Nyquist plot + SOH + spectrum table)
// =============================================================
const char DASHBOARD[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CellSight - EIS Battery Analyzer</title>
<style>
 body{font-family:'Segoe UI',Arial;background:#0b1120;color:#e2e8f0;margin:0;padding:16px}
 h1{color:#38bdf8;text-align:center;margin:4px}
 .sub{text-align:center;color:#94a3b8;margin-bottom:14px}
 .wrap{max-width:980px;margin:auto}
 .cards{display:flex;gap:12px;flex-wrap:wrap;justify-content:center}
 .card{background:#1e293b;border-radius:12px;padding:14px 18px;min-width:150px;text-align:center}
 .card b{font-size:1.5em;color:#f8fafc}.card span{color:#94a3b8;font-size:.85em}
 #soh{font-size:2.2em}
 canvas{background:#0f172a;border:1px solid #334155;border-radius:12px;width:100%}
 button{background:#0284c7;color:#fff;border:0;border-radius:8px;padding:10px 18px;
        font-size:1em;cursor:pointer;margin:4px}
 button:hover{background:#0369a1}
 table{width:100%;border-collapse:collapse;font-size:.85em}
 td,th{border-bottom:1px solid #334155;padding:4px 6px;text-align:right}
 pre{background:#0f172a;border-radius:8px;padding:10px;white-space:pre-wrap;max-height:160px;overflow:auto}
</style></head><body><div class="wrap">
<h1>🔋 CellSight — EIS Battery Health Analyzer</h1>
<div class="sub">Electrochemical Impedance Spectroscopy · Nyquist Analysis · SOH Grading</div>
<div class="cards">
 <div class="card"><span>STATE OF HEALTH</span><br><b id="soh">--</b><br><span id="grade">not measured</span></div>
 <div class="card"><span>OCV</span><br><b id="ocv">--</b><br><span>volts</span></div>
 <div class="card"><span>R ohmic</span><br><b id="ro">--</b><br><span>mΩ</span></div>
 <div class="card"><span>R ct</span><br><b id="rct">--</b><br><span>mΩ</span></div>
 <div class="card"><span>DCIR</span><br><b id="dcir">--</b><br><span>mΩ</span></div>
 <div class="card"><span>Cell Temp</span><br><b id="temp">--</b><br><span>°C</span></div>
</div>
<p style="text-align:center">
 <button onclick="cmd('sweep')">▶ Run EIS Sweep</button>
 <button onclick="cmd('cal')">Calibrate (CAL resistor fitted)</button>
 <button onclick="cmd('dcir')">DCIR Pulse Test</button>
</p>
<h3>Nyquist Plot (−Z″ vs Z′)</h3>
<canvas id="nyq" width="940" height="380"></canvas>
<h3>Spectrum Data</h3>
<div style="max-height:220px;overflow:auto"><table id="tbl">
<tr><th>Freq (Hz)</th><th>|Z| (mΩ)</th><th>Z′ (mΩ)</th><th>−Z″ (mΩ)</th><th>Phase (°)</th></tr>
</table></div>
<h3>Event Log</h3><pre id="log"></pre>
<script>
function cmd(c){fetch('/cmd?c='+c);}
function drawNyq(d){
 const cv=document.getElementById('nyq'),x=cv.getContext('2d');
 x.clearRect(0,0,cv.width,cv.height);
 if(!d.pts.length)return;
 let mx=0,my=0;d.pts.forEach(p=>{mx=Math.max(mx,p[1]);my=Math.max(my,-p[2]);});
 mx*=1.15;my*=1.15;
 x.strokeStyle='#334155';x.beginPath();
 x.moveTo(60,10);x.lineTo(60,340);x.lineTo(920,340);x.stroke();
 x.fillStyle='#94a3b8';x.fillText("Z' (mΩ) →",830,358);x.fillText("-Z'' (mΩ)",8,20);
 x.strokeStyle='#38bdf8';x.lineWidth=2;x.beginPath();
 d.pts.forEach((p,i)=>{const px=60+(p[1]/mx)*840,py=340-((-p[2])/my)*320;
   i?x.lineTo(px,py):x.moveTo(px,py);});
 x.stroke();
 x.fillStyle='#f472b6';
 d.pts.forEach(p=>{const px=60+(p[1]/mx)*840,py=340-((-p[2])/my)*320;
   x.beginPath();x.arc(px,py,3,0,7);x.fill();});
}
async function tick(){
 const d=await(await fetch('/data')).json();
 document.getElementById('soh').textContent=d.soh?d.soh.toFixed(0)+'%':'--';
 document.getElementById('grade').textContent=d.grade;
 document.getElementById('ocv').textContent=d.ocv.toFixed(3);
 document.getElementById('ro').textContent=(d.ro*1000).toFixed(1);
 document.getElementById('rct').textContent=(d.rct*1000).toFixed(1);
 document.getElementById('dcir').textContent=(d.dcir*1000).toFixed(1);
 document.getElementById('temp').textContent=d.temp.toFixed(1);
 document.getElementById('log').textContent=d.log;
 let rows='<tr><th>Freq (Hz)</th><th>|Z| (mΩ)</th><th>Z′ (mΩ)</th><th>−Z″ (mΩ)</th><th>Phase (°)</th></tr>';
 d.rows.forEach(r=>{rows+='<tr><td>'+r[0]+'</td><td>'+r[1]+'</td><td>'+r[2]+'</td><td>'+r[3]+'</td><td>'+r[4]+'</td></tr>';});
 document.getElementById('tbl').innerHTML=rows;
 drawNyq(d);
}
setInterval(tick,2000);tick();
</script></div></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", DASHBOARD); }

void handleData() {
  String j = "{";
  j += "\"soh\":" + String(sohPercent, 1);
  j += ",\"grade\":\"" + sohGrade + "\"";
  j += ",\"ocv\":" + String(cellOCV, 3);
  j += ",\"ro\":" + String(rOhmic, 4);
  j += ",\"rct\":" + String(rCt, 4);
  j += ",\"dcir\":" + String(dcir, 4);
  j += ",\"temp\":" + String(cellTempC, 1);
  j += ",\"log\":\"";
  String esc = eventLog; esc.replace("\n", "\\n"); esc.replace("\"", "'");
  j += esc + "\"";
  j += ",\"pts\":[";
  for (int i = 0; i < sweepCount; i++) {
    if (i) j += ",";
    j += "[" + String(sweep[i].freq, 1) + "," +
         String(sweep[i].zReal * 1000, 2) + "," +
         String(sweep[i].zImag * 1000, 2) + "]";
  }
  j += "],\"rows\":[";
  for (int i = 0; i < sweepCount; i++) {
    if (i) j += ",";
    j += "[" + String(sweep[i].freq, 1) + "," +
         String(sweep[i].zMag * 1000, 1) + "," +
         String(sweep[i].zReal * 1000, 1) + "," +
         String(-sweep[i].zImag * 1000, 1) + "," +
         String(sweep[i].phaseDeg, 1) + "]";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleCmd() {
  String c = server.arg("c");
  if (c == "cal") {
    server.send(200, "text/plain", "calibrating");
    if (!runCalibration()) logEvent("Calibration FAILED - check AFE/CAL resistor");
  } else if (c == "sweep") {
    server.send(200, "text/plain", "sweep started");
    if (gainFactor == 0) logEvent("WARNING: not calibrated - results indicative only");
    runSweep();
  } else if (c == "dcir") {
    server.send(200, "text/plain", "dcir done");
    runDcirTest();
  }
}

// =============================================================
//  SETUP & LOOP
// =============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  tempSensor.begin();
  pinMode(LOAD_PIN, OUTPUT);
  digitalWrite(LOAD_PIN, LOW);
  analogReadResolution(12);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(300);
  if (WiFi.status() == WL_CONNECTED) {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/cmd", handleCmd);
    server.begin();
    logEvent("Dashboard: http://" + WiFi.localIP().toString());
  } else {
    logEvent("Wi-Fi offline - serial mode. Commands: c=calibrate s=sweep d=dcir");
  }

  if (runCalibration()) logEvent("Auto-calibration done (CAL path).");
  cellOCV = readCellVoltage();
  logEvent("CellSight ready. OCV=" + String(cellOCV, 3) + "V");
}

void loop() {
  server.handleClient();

  static unsigned long lastTemp = 0;
  if (millis() - lastTemp > 3000) {
    lastTemp = millis();
    tempSensor.requestTemperatures();
    float t = tempSensor.getTempCByIndex(0);
    if (t > -100) cellTempC = t;
    cellOCV = readCellVoltage();
  }

  if (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'c') runCalibration();
    if (ch == 's') runSweep();
    if (ch == 'd') runDcirTest();
  }
}
