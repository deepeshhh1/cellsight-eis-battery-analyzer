# Reading a Nyquist Plot — CellSight Field Guide

A Nyquist plot graphs **−Z″ (imaginary impedance, y-axis)** against **Z′ (real impedance, x-axis)**, one point per test frequency. High frequencies sit on the **left**, low frequencies on the **right**.

```
 -Z''
   ^
   |        .- - -.
   |      /         \            /
   |     /  R_ct     \          /  Warburg tail (45°)
   |    /  semicircle \        /
   |---●---------------●------●----------> Z'
   |  R_ohmic        crossing
```

## The three regions

1. **High-frequency intercept (leftmost point)** — where the curve meets the Z′ axis. This is **R_ohmic**: electrolyte, separator and contact resistance. It is the single most reliable ageing indicator — a cell whose R_ohmic has doubled has lost a large part of its power capability.
2. **Mid-frequency semicircle** — its diameter is **R_ct**, the charge-transfer resistance at the electrode surfaces. The semicircle grows and flattens as electrodes degrade and SEI layers thicken.
3. **Low-frequency tail** — a ~45° line is the **Warburg** diffusion element: lithium ions diffusing inside the electrode particles. A tail that bends sharply upward suggests diffusion blockage.

## Grading rules used by CellSight

| SOH | Grade | Verdict |
| --- | --- | --- |
| ≥ 90% | A | Excellent — full EV duty |
| 80–90% | B | Good — EV duty |
| 70–80% | C | Second life — stationary storage |
| 60–70% | D | Second life — light duty only |
| < 60% | E | Recycle |

SOH is scored 60% on R_ohmic growth and 40% on R_ct growth versus the stored fresh-cell baseline, then cross-checked with the DCIR pulse test — if AC and DC disagree wildly, suspect a loose contact or an uncalibrated front-end before condemning the cell.
