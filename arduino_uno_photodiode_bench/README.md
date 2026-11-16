# Photodiode box-sensor bench — Arduino Uno

A standalone copy of the ESP32-C3 photodiode bench, adapted for the **Arduino Uno**
and the **Arduino IDE** (no PlatformIO, no Python). Copy this whole folder to the
computer where you'll run the Arduino IDE — it's self-contained.

## Wiring

```
5V ──[ photodiode / phototransistor ]──●──[ fixed resistor ]── GND
                                       │
                                      A0
```

- The light sensor sits on the **high side**; the midpoint feeds **A0**.
- More light → the sensor conducts more → the midpoint voltage rises → a higher `raw`.
- 10 kΩ–100 kΩ is a good starting resistor (larger resistor = bigger swing for a
  bare photodiode, which passes only tiny currents).
- If the numbers move the *wrong way* (open reads lower), your divider is flipped
  — swap the sensor and resistor.

## Use in the Arduino IDE

1. Open `arduino_uno_photodiode_bench.ino` in the Arduino IDE.
2. **Tools → Board → Arduino Uno**; **Tools → Port** → your Uno's serial port.
3. Upload, then open **Tools → Serial Monitor** at **115200 baud**.
4. Cover/uncover the sensor and watch the second CSV column (`raw`) move.
5. Send `t=<n>` (e.g. `t=512`) to change the open/closed threshold live.
6. To record a run, tick **"Show timestamp"** in the Serial Monitor and copy-paste
   the output to a text file.

## Output

One line per sample, same fields as the ESP32 bench:

```
millis,raw,state,min,max,mean,cycles,threshold
```

`raw` is the 10-bit ADC reading (0..1023); `state` is 1 (open) when `raw > threshold`.

## Differences vs the ESP32-C3 bench

|                  | Arduino Uno          | ESP32-C3 bench          |
|------------------|----------------------|-------------------------|
| ADC resolution   | 10-bit → **0..1023** | 12-bit → **0..4095**    |
| ADC reference    | ~5 V                 | ~3.3 V                  |
| threshold range  | 0..1023              | 0..4095                 |

**The threshold does not transfer between boards.** The Uno bench is for
validating the sensor + divider wiring and measuring the open-vs-closed
separation. Re-run the ESP32-C3 `bench-photodiode` to calibrate the actual value
that goes into `CLIP_SENSOR_LIGHT_THRESHOLD`.
