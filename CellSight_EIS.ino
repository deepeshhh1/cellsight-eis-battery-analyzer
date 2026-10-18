// ============================================================
//  CELLSIGHT - EIS BATTERY HEALTH ANALYZER (v2, multi-file)
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
//  PROJECT STRUCTURE (v2)
//    v1 of CellSight was a single 531-line sketch. v2 keeps
//    every v1 feature and contract, but splits the code into
//    focused modules so each layer can be read, tested and
//    reused on its own. The folder compiles as ONE Arduino
//    sketch - open this .ino and the IDE pulls in both headers:
//
//      CellSight_EIS.ino (this file)
//        Owns the hardware and the application: pin map, cell
//        temperature (DS18B20), OCV + DCIR DC measurements,
//        the event log, and the web server with its dashboard
//        and the /, /data, /cmd and /csv endpoints.
//      ad5933.h
//        Register-level driver for the AD5933 impedance
//        converter: I2C primitives, frequency programming,
//        control commands and raw DFT readout.
//      eis.h
//        The measurement engine: calibration, the averaged
//        and temperature-compensated frequency sweep, Nyquist
//        feature extraction and SOH grading. It calls back
//        into this sketch only through logEvent(),
//        serviceClients() and the shared cellTempC value.
//
//    New in v2: per-point averaging (3 conversions per
//    frequency) with a per-point repeatability (spread)
//    figure, temperature compensation of the spectrum to
//    the 25 degC reference, CSV export of the last sweep
//    from the dashboard (GET /csv), and the same CSV as a
//    serial report for offline bench work.
//
//  Author : Deepesh Chauhan, B.Tech Electrical Engineering,
//           RGGEC Kangra
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include "ad5933.h"      // AD5933 register-level driver
#include "eis.h"         // calibration, sweep engine, analysis

// ---------------- WI-FI (replace with your credentials) ------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ---------------- PINS --------------------------------------
#define TEMP_PIN     4                   // DS18B20 data
#define LOAD_PIN     26                  // DC load-pulse MOSFET
#define CELL_ADC_PIN 34                  // cell voltage divider

// Cell voltage divider: cell (max 4.2V) -> 2:1 divider -> ADC
const float CELL_DIV_RATIO = 2.0f;

// ---------------- SKETCH-OWNED STATE --------------------------
// cellTempC is declared extern in eis.h: the sweep engine reads
// it for temperature compensation and snapshots it per sweep.
float cellTempC = 25.0f;                 // DS18B20 cell temperature
float cellOCV   = 0;                     // last open-circuit voltage
float dcir      = 0;                     // last DCIR result, ohms
String eventLog = "Boot\n";              // dashboard / serial log

// ---------------- OBJECTS -------------------------------------
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
WebServer server(80);

// =============================================================
//  LOGGING + WEB PUMP (services consumed by eis.h)
// =============================================================
void logEvent(const String& msg) {
  eventLog += msg + "\n";
  if (eventLog.length() > 1600) eventLog = eventLog.substring(500);
  Serial.println(msg);
}

// eis.h calls this inside the sweep loop: a full sweep can take
// minutes at the low-frequency end, and the web server must
// keep answering the dashboard meanwhile.
void serviceClients() {
  server.handleClient();
}

// =============================================================
//  DC MEASUREMENTS - OCV + DCIR LOAD PULSE
//  The AC (EIS) picture is cross-checked with two DC tests:
//  the rested open-circuit voltage, and the voltage sag under
//  a known 1-second load pulse (DC internal resistance).
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
//  DASHBOARD  (Nyquist plot + SOH + spectrum table + CSV)
//  Single self-contained page served from flash. It polls
//  /data every 2 s for the JSON state, fires actions at /cmd,
//  and downloads the last sweep from /csv.
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
 <button onclick="location.href='/csv'">⬇ Download CSV</button>
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

// =============================================================
//  HTTP ENDPOINT REFERENCE
//    GET /            this dashboard (single-page UI)
//    GET /data        JSON state: SOH/grade, OCV, R_ohmic,
//                     R_ct, DCIR, cell temp, event log, and
//                     the spectrum as plot points + table rows
//    GET /cmd?c=...   actions: cal | sweep | dcir (see below)
//    GET /csv         last sweep as a CSV file download,
//                     25 degC-compensated values (see below)
// =============================================================
void handleRoot() { server.send_P(200, "text/html", DASHBOARD); }

// =============================================================
//  /data - JSON state for the dashboard (contract as v1).
//  Impedance values are the temperature-compensated spectrum
//  stored by eis.h; the dashboard works in milliohms.
// =============================================================
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

// =============================================================
//  /csv - stream the last sweep as a CSV download (new in v2).
//  One row per frequency point. The impedance columns are the
//  25 degC-compensated values in milliohms; z_imag_mohm is the
//  signed imaginary part (negative = capacitive, as stored).
//  cell_temp_c is the temperature the sweep was corrected
//  from, and timestamp_ms is the device uptime (millis) at
//  sweep completion - both are properties of the sweep, so
//  they repeat on every row. With no sweep yet, the response
//  is just the header row. The body is streamed in chunks
//  (unknown content length) rather than buffered whole.
// =============================================================
void handleCsv() {
  server.sendHeader("Content-Disposition",
                    "attachment; filename=\"cellsight_sweep.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent(
      "freq_hz,z_mag_mohm,z_real_mohm,z_imag_mohm,phase_deg,"
      "cell_temp_c,timestamp_ms\n");
  for (int i = 0; i < sweepCount; i++) {
    const EisPoint& p = sweep[i];
    String line;
    line += String(p.freq, 1);         line += ",";
    line += String(p.zMag * 1000, 2);  line += ",";
    line += String(p.zReal * 1000, 2); line += ",";
    line += String(p.zImag * 1000, 2); line += ",";
    line += String(p.phaseDeg, 2);     line += ",";
    line += String(sweepTempC, 1);     line += ",";
    line += String(sweepTimestampMs);  line += "\n";
    server.sendContent(line);
  }
  server.sendContent("");                // terminate chunked body
}

// =============================================================
//  SERIAL SWEEP REPORT (new in v2)
//  Dumps the last sweep to the serial console in the same CSV
//  layout as /csv, so a bench session without Wi-Fi can still
//  capture the spectrum into any serial terminal and save it
//  straight into a spreadsheet or an EIS fitting tool.
// =============================================================
void printSweepSerial() {
  Serial.println("freq_hz,z_mag_mohm,z_real_mohm,z_imag_mohm,"
                 "phase_deg,spread_pct,cell_temp_c,timestamp_ms");
  for (int i = 0; i < sweepCount; i++) {
    const EisPoint& p = sweep[i];
    Serial.print(String(p.freq, 1));         Serial.print(",");
    Serial.print(String(p.zMag * 1000, 2));  Serial.print(",");
    Serial.print(String(p.zReal * 1000, 2)); Serial.print(",");
    Serial.print(String(p.zImag * 1000, 2)); Serial.print(",");
    Serial.print(String(p.phaseDeg, 2));     Serial.print(",");
    Serial.print(String(p.spreadPct, 2));    Serial.print(",");
    Serial.print(String(sweepTempC, 1));     Serial.print(",");
    Serial.println(String(sweepTimestampMs));
  }
}

// =============================================================
//  /cmd - dashboard actions (contract as v1):
//    c=cal -> calibration, c=sweep -> EIS sweep, c=dcir -> DCIR
// =============================================================
void handleCmd() {
  String c = server.arg("c");
  if (c == "cal") {
    server.send(200, "text/plain", "calibrating");
    if (!runCalibration()) logEvent("Calibration FAILED - check AFE/CAL resistor");
  } else if (c == "sweep") {
    server.send(200, "text/plain", "sweep started");
    if (gainFactor == 0) logEvent("WARNING: not calibrated - results indicative only");
    runSweep();
    printSweepSerial();
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
  Serial.println();
  Serial.println("CellSight EIS Battery Health Analyzer v2 (multi-file)");
  Serial.println("ESP32 + AD5933 + DS18B20 - averaged, temperature-"
                 "compensated EIS with SOH grading");
  Wire.begin(21, 22);
  if (adSelfTest()) {
    logEvent("AD5933 self-test passed (register readback OK).");
  } else {
    logEvent("WARNING: AD5933 not responding - check I2C wiring/pull-ups "
             "before trusting any measurement.");
  }
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
    server.on("/csv", handleCsv);
    server.begin();
    logEvent("Dashboard: http://" + WiFi.localIP().toString());
  } else {
    logEvent("Wi-Fi offline - serial mode. Commands: c=calibrate s=sweep d=dcir p=print sweep");
  }

  if (runCalibration()) logEvent("Auto-calibration done (CAL path).");
  cellOCV = readCellVoltage();
  logEvent("CellSight ready. OCV=" + String(cellOCV, 3) + "V");
}

void loop() {
  server.handleClient();

  // Refresh cell temperature + OCV every 3 s. The temperature
  // feeds both the dashboard and eis.h's compensation.
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
    if (ch == 's') { runSweep(); printSweepSerial(); }
    if (ch == 'd') runDcirTest();
    if (ch == 'p') printSweepSerial();
  }
}
