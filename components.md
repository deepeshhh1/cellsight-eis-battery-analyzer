# Components — CellSight EIS Battery Analyzer

| S.No. | Component | Purpose | Qty |
| --- | --- | --- | --- |
| 1 | ESP32 DevKit V1 (30-pin) | Main controller + Wi-Fi dashboard | 1 |
| 2 | AD5933 Impedance Converter Module | Frequency sweep generation + DFT impedance measurement | 1 |
| 3 | Battery EIS Analog Front-End (AFE) | Conditions the AD5933 signal to a safe ±10 mV perturbation on the cell; gain/offset staging | 1 |
| 4 | Precision Resistor 10 Ω (0.1%) | Calibration reference for gain factor / phase | 1 |
| 5 | DS18B20 Temperature Sensor | Cell surface temperature during tests | 1 |
| 6 | Logic-level N-MOSFET (e.g. IRLZ44N) | Switches the DCIR load pulse | 1 |
| 7 | Power Resistor ~1.4 Ω / 20 W | DCIR pulse load (≈3 A at 4.2 V) | 1 |
| 8 | 18650 Cell Holder + Test Cells | Cells under test (new + aged cells for grading comparison) | 1 set |
| 9 | Voltage Divider (2 × 10 kΩ) | Cell voltage sensing into ESP32 ADC | 1 set |
| 10 | 5 V 2 A Power Adapter | Power for ESP32 and modules | 1 |
| 11 | Breadboard / PCB + Wiring | Assembly | 1 set |

**Calibration discipline:** run the calibration routine with the precision resistor fitted in place of the cell before every measurement session, and record a fresh-cell baseline before grading aged cells.
