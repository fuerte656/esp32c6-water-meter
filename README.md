# ESP32-C6 Zigbee Water Meter - Build Guide

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
idf.py build
idf.py -p COM4 flash monitor
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
Estimated runtime on 3xAA alkaline: ~25 h. Useful as an intermediate
test before deep-sleep, since the network rejoin behavior is the
same as deep-sleep mode but without the sleep-wake state machine.

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

Behavior: chip sleeps in deep sleep until a pulse on GPIO 2 OR every
WM_KEEPALIVE_PERIOD_S (default 15 min). On wake, joins parent, sends
report, sleeps. ~50 uA average. Estimated 6-12 months on 3xAA
lithium cells.

## Battery monitoring (optional, requires hardware)

To enable battery percentage reporting, wire the voltage divider:

```
Battery+ ----[100k ohm]----+----[100k ohm]---- GND
                           |
                        GPIO 4
```

Then set `WM_BATTERY_MONITORING = 1` in wm_config.h and rebuild. The
device will expose a `battery` sensor in HA.

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
  report. Cumulative pulse counter lives in RTC slow memory so it
  survives across wakes.
- Don't reduce WM_KEEPALIVE_PERIOD_S below ~5 min - it just burns
  battery for negligible benefit (HA already knows the device exists
  via the last seMetering report).
- After flashing, the first boot does a full network steering. Hold
  the device near the coordinator (~30 cm) to ensure clean join.
