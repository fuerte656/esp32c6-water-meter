# ESP32-C6 Zigbee Water Meter - Build Guide

This firmware supports **two independent water meters** on a single
ESP32-C6:

- Meter 1: reed switch on **GPIO 2** -> GND, Zigbee endpoint **10**
- Meter 2: reed switch on **GPIO 5** -> GND, Zigbee endpoint **11**

Both share the same pulse weight (`WM_LITERS_PER_PULSE_X1000`). The
Z2M converter in `z2m_converter/water_meter.js` exposes each meter as
`water_consumed_meter1` / `water_consumed_meter2` (m³) plus the
matching `_liters` variants.

## Operating modes

The firmware can be built in three modes by toggling flags in
`main/wm_config.h` AND choosing the right sdkconfig.defaults file(s)
on the build command line.

### 1. USB / Router (default - currently working)

```c
// main/wm_config.h
#define WM_POWER_USB             1
#define WM_DEEP_SLEEP            0
#define WM_BATTERY_MONITORING    0
```

```bash
idf.py fullclean
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.battery" build
idf.py -p COM4 build erase-flash flash monitor
```

Behavior: always-on Zigbee Router, ~80 mA continuous, full mesh
participation, fastest response.

### 2. Battery / End Device (always-on, no sleep)

```c
// main/wm_config.h
#define WM_POWER_USB             0
#define WM_DEEP_SLEEP            0
#define WM_BATTERY_MONITORING    0     // or 1 if hardware ready
```

```bash
idf.py fullclean
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.battery" build
idf.py -p COM4 erase-flash
idf.py -p COM4 flash monitor
```

Behavior: Zigbee End Device, always awake, ~50 mA continuous.
Useful only as a bench test before deep-sleep, since the network
rejoin behavior is the same as deep-sleep mode but without the
sleep-wake state machine. **Not viable for real battery deployment**:
a fresh LS14500 (~2.6 Ah) would last ~50 hours at 50 mA.

### 3. Battery / Deep Sleep

```c
// main/wm_config.h
#define WM_POWER_USB             0
#define WM_DEEP_SLEEP            1
#define WM_BATTERY_MONITORING    0     // or 1 if hardware ready
```

```bash
idf.py fullclean
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.battery" build
idf.py -p COM4 erase-flash
idf.py -p COM4 flash monitor
```

Behavior: chip sleeps in deep sleep until a pulse on GPIO 2 / GPIO 5
OR every WM_KEEPALIVE_PERIOD_S (default 1 hour). On wake, joins
parent, sends reports for both meters, sleeps. ~50 uA average.
Estimated **~5 years** on a single LS14500 (LiSOCl2 AA, ~2.6 Ah) +
HPC1550 hybrid capacitor; the HPC supplies the Zigbee TX bursts the
bare cell cannot.

## Hardware: XIAO ESP32-C6 power path

The target board is the **Seeed Studio XIAO ESP32-C6**, which uses
two SG Micro power-management ICs near the BAT+/BAT- pads:

- **SGM40567-4.2** — single-cell Li-Ion linear battery charger with
  integrated power-path management. The `-4.2` suffix is the charge
  termination voltage (4.2 V, standard for Li-Ion / LiPo). When USB
  is plugged the IC routes 5 V to the system rail AND pushes a CC/CV
  charge current into whatever's connected to BAT+. When USB is
  unplugged the system seamlessly draws from BAT+.
- **SGM6029C** — synchronous step-down (buck) DC-DC converter that
  regulates the battery / USB rail down to the 3.3 V the ESP32-C6
  runs on. Much more efficient than the simple LDO found on older
  XIAO siblings, which is what lets the C6 reach sub-microamp sleep
  numbers when battery-powered.

### ⚠️ Warning when using LS14500 (or any primary cell)

The SGM40567-4.2 **actively charges** whatever's wired to BAT+. That
is fine for Li-Ion / LiPo cells, **but dangerous for the LS14500
LiSOCl2 cell recommended below** — primary cells must never be
charged. Connecting an LS14500 directly to BAT+ as-is can cause the
cell to vent, leak corrosive electrolyte, or rupture.

Before connecting an LS14500, choose one of:

1. **Disable / desolder the SGM40567-4.2** (recommended). After
   removal the BAT+ pad becomes a passive pad — wire LS14500 +
   HPC1550 directly. This is also what the Seeed forum thread
   suggests for primary-cell builds.
2. **Series Schottky** between cell and BAT+ to block reverse
   (charging) current. Cheap but leaves the charger trying to drive
   an open output; use a low-leakage Schottky (e.g. PMEG2010).
3. **Bypass the charger** entirely with an external ideal-diode IC
   (LM66100) feeding the 3.3 V rail, leaving BAT+/BAT- unconnected.

Rechargeable Li-Ion / LiPo cells need none of this — they work
straight out of the box.

## Battery monitoring (optional, requires hardware)

Recommended cell: **1x LS14500** (Saft LiSOCl2 AA, 3.6 V, ~2.6 Ah) in
parallel with **1x HPC1550** (Tadiran hybrid layer capacitor, ~40 mAh)
to buffer Zigbee TX current bursts. Make sure you've handled the
SGM40567-4.2 charger as described in the previous section before
connecting the cell.

To enable battery reporting, wire the voltage divider:

```
Battery+ ----[100k ohm]----+----[100k ohm]---- GND
                           |
                        GPIO 4
```

Then set `WM_BATTERY_MONITORING = 1` in wm_config.h and rebuild. The
device will expose two attributes via the Zigbee PowerCfg cluster:

- **BatteryVoltage** (raw mV, the most meaningful health signal for
  LiSOCl2 since the chemistry is flat near 3.6 V for years).
- **BatteryPercentageRemaining**, derived from voltage in discrete
  buckets (100/70/40/15/5/0 %) because a linear percentage on a flat
  curve is misleading.

Replace the cell when voltage drops below ~3.3 V — that's the knee of
the LiSOCl2 discharge curve; from there the cell typically lasts
weeks, not months.

ADC samples are averaged (16 reads spaced ~2 ms) so brief rail dips
during Zigbee TX bursts don't corrupt the reading.

## Switching between modes

Always:
1. Edit wm_config.h
2. `idf.py fullclean`
3. `idf.py build` (with right SDKCONFIG_DEFAULTS)
4. `idf.py erase-flash` (Zigbee role change requires fresh credentials)
5. `idf.py flash monitor`
6. Open permit-join in Z2M
7. Wait for rejoin

After mode change, the device leaves and rejoins the network. Any
existing Z2M friendly_name and configuration is preserved (matched by
IEEE address). HA discovery republishes automatically.

## Recommended testing order

1. **USB defaults** - verify nothing regressed. Should match today's
   working behavior exactly.
2. **Battery / always-on** - tests End Device join and report flow
   without sleep complications. Easier to debug than deep sleep.
3. **Battery / deep sleep** - real prize, but most likely to need
   debugging. Watch the rejoin-on-wake timing carefully.
4. **Battery monitoring** - last, after all the above are stable.
   Just adds an ADC read + extra cluster.

## Pitfalls

- The PCNT peripheral is **off during deep sleep**. Pulses arriving
  during the ~50 ms sleep-entry window are lost. For typical home
  water flow (a few pulses per minute max) this is rare.
- Each deep-sleep wake takes ~3-4 seconds to rejoin parent + send
  report. Both cumulative counters live in RTC slow memory so they
  survive across wakes.
- In deep sleep both reed-switch GPIOs are configured as wake-on-low
  sources. On wake, each line is sampled - whichever is still closed
  is credited with one pulse. If both fire simultaneously, both get
  credited.
- The supervisor forces a Zigbee report on every meter (and the
  battery, if monitored) every `WM_KEEPALIVE_PERIOD_S` (1 hour by
  default), so Home Assistant's availability tracker stays happy
  even on dry days. In deep-sleep mode the chip wakes on the same
  timer to do the same thing.
- Don't reduce WM_KEEPALIVE_PERIOD_S below ~5 min - it just burns
  battery for negligible benefit (HA already knows the device exists
  via the last seMetering report).
- After flashing, the first boot does a full network steering. Hold
  the device near the coordinator (~30 cm) to ensure clean join.
