# CellSight — EIS Battery Health Analyzer 🔋🔬

**Electrochemical Impedance Spectroscopy (EIS) battery diagnostics with Nyquist-plot analysis and State-of-Health grading — the technique used in battery research labs and EV battery programs.**

Most student BMS projects stop at voltage and temperature. CellSight goes to the lab technique: it injects a small AC perturbation into a Li-ion cell, sweeps it from **10 Hz to 100 kHz**, measures the complex impedance at every point, and extracts the parameters that actually decide whether a cell is fit for an EV, fit for second-life storage, or fit only for recycling.

## 📈 What it measures

| Parameter | Meaning | Why it matters |
| --- | --- | --- |
| **R_ohmic** | High-frequency Nyquist intercept | Electrolyte + contact resistance — rises as the cell ages/dries |
| **R_ct** | Charge-transfer semicircle diameter | Electrode surface degradation |
| **Warburg slope** | Low-frequency diffusion tail | Lithium diffusion health inside the electrodes |
| **DCIR** | DC load-pulse internal resistance | Cross-check of the AC measurement |
| **SOH grade** | A (EV duty) → E (recycle) | Combined score from R_ohmic + R_ct growth vs a fresh-cell baseline |

## 🔍 How it works

1. The **AD5933** impedance converter generates the sweep and runs the on-chip DFT at each frequency
2. The cell is perturbed through an **analog front-end (AFE)** — only ±10 mV AC around its rest potential, never a raw connection
3. Calibration against a precision resistor yields the system gain factor and phase offset
4. Firmware reconstructs Z′/Z″, draws the **Nyquist plot** on the web dashboard, and grades SOH
5. A **DCIR pulse test** (MOSFET-switched known load) and DS18B20 temperature complete the picture
6. Each sweep point is measured **3× and averaged** (circular phase mean), impedance is **temperature-compensated** to the 25 °C reference, and the full spectrum can be **exported as CSV** from the dashboard

## 📂 In this repo (multi-file firmware)

- `CellSight_EIS.ino` — main sketch: dashboard, DCIR test, OCV/temperature, CSV export endpoint
- `ad5933.h` — complete AD5933 impedance-converter driver
- `eis.h` — sweep engine, calibration, per-point averaging, temperature compensation, Nyquist analysis + SOH grading
- `components.md` — component list with the role of each part
- `nyquist-explained.md` — how to read a Nyquist plot and judge a cell from it

## 🖥️ Dashboard

Dark web dashboard served by the ESP32: SOH gauge with grade, OCV, R_ohmic, R_ct, DCIR, cell temperature, a live **Nyquist plot canvas**, the full spectrum table, buttons to run a sweep / calibration / DCIR test, and a **Download CSV** export of the last sweep.

## 📚 Required libraries

Board support (once): **esp32 by Espressif Systems** — Arduino IDE → Boards Manager.

| Library | Used for | Install (Library Manager search) |
| --- | --- | --- |
| OneWire (Paul Stoffregen) | DS18B20 1-Wire temperature bus | `OneWire` |
| DallasTemperature (Miles Burton) | DS18B20 temperature readings | `DallasTemperature` |

Built into the ESP32 core, no install needed: `WiFi`, `WebServer`, `Wire`. The AD5933 driver is written in this repo (`ad5933.h`) — no external AD5933 library is required.

## 🔌 Wiring — pin connections (ESP32)

| ESP32 pin | Connects to | Notes |
| --- | --- | --- |
| GPIO21 (SDA) | AD5933 SDA | I2C bus — AD5933 fixed address `0x0D` |
| GPIO22 (SCL) | AD5933 SCL | I2C clock |
| AD5933 VOUT / VIN | Cell under test, via the analog front-end (AFE) | ±10 mV AC perturbation around rest potential — never connect the AD5933 directly across a raw cell |
| GPIO4 | DS18B20 data (DQ) | Add a 4.7 kΩ pull-up to 3.3 V |
| GPIO26 | DCIR load-pulse MOSFET gate | Switches the known load resistor across the cell for the DC-pulse test |
| GPIO34 | Cell voltage divider output | ADC input — cell OCV sensing |
| GND | AFE / sensor ground | Common ground throughout |

## ⚠️ Safety

Li-ion cells store serious energy. The EIS front-end must isolate and limit the perturbation current; the load-pulse test uses a properly rated resistor and MOSFET. Never short a cell, never exceed its voltage limits, and supervise all tests.

## 👤 Author

**Deepesh Chauhan** — B.Tech Electrical Engineering, RGGEC Kangra
[LinkedIn](https://www.linkedin.com/in/deepeshhh1)
