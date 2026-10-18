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

## 📂 In this repo

- `CellSight_EIS.ino` — complete firmware (AD5933 driver, sweep engine, Nyquist analysis, SOH grading, dashboard)
- `components.md` — component list with the role of each part
- `nyquist-explained.md` — how to read a Nyquist plot and judge a cell from it

## 🖥️ Dashboard

Dark web dashboard served by the ESP32: SOH gauge with grade, OCV, R_ohmic, R_ct, DCIR, cell temperature, a live **Nyquist plot canvas**, the full spectrum table, and buttons to run a sweep / calibration / DCIR test.

## ⚠️ Safety

Li-ion cells store serious energy. The EIS front-end must isolate and limit the perturbation current; the load-pulse test uses a properly rated resistor and MOSFET. Never short a cell, never exceed its voltage limits, and supervise all tests.

## 👤 Author

**Deepesh Chauhan** — B.Tech Electrical Engineering, RGGEC Kangra
[LinkedIn](https://www.linkedin.com/in/deepeshhh1)
