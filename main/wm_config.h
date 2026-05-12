#pragma once

/* =================================================================
 * ESP32-C6 Zigbee Water Meter - configuration
 * =================================================================
 *
 * Hardware: ESP32-C6 + reed switches on GPIO 2 and GPIO 5 (to GND).
 * Optional: 1x LS14500 (LiSOCl2 AA, 3.6 V) + 1x HPC1550 (Tadiran
 *           hybrid layer capacitor) in parallel for battery operation.
 *           LiSOCl2 sits in spec for ESP32-C6 Vdd (3.0-3.6 V) for the
 *           whole life of the cell, so no buck-boost is required; a
 *           low-Iq LDO is optional for spike protection.
 * Optional: voltage divider on GPIO 4 (100k+100k) for battery sensing.
 *
 * Flip the flags below to choose the operating mode. Defaults match
 * the working USB-powered router build.
 */

/* =================================================================
 * Power mode
 * =================================================================
 *
 *   WM_POWER_USB = 1  -> mains/USB powered, runs as Zigbee Router,
 *                       always on, full mesh participation, fastest
 *                       response. ~80 mA continuous.
 *
 *   WM_POWER_USB = 0  -> battery powered, runs as Zigbee End Device.
 *                       See WM_DEEP_SLEEP below for sleep behaviour.
 */
#define WM_POWER_USB             0

/* =================================================================
 * Deep sleep (only used when WM_POWER_USB = 0)
 * =================================================================
 *
 *   WM_DEEP_SLEEP = 0 -> always-on End Device. Lower power than
 *                       Router (~50 mA), but still always awake.
 *                       ~25 h on 3xAA alkaline.
 *
 *   WM_DEEP_SLEEP = 1 -> deep sleep between pulses. Wakes on GPIO 2
 *                       falling edge, sends report, sleeps. Also
 *                       wakes every WM_KEEPALIVE_PERIOD_S for a
 *                       heartbeat. ~50 uA average.
 *                       Estimated 6-12 months on 3xAA lithium.
 */
#define WM_DEEP_SLEEP            0

/* =================================================================
 * Battery monitoring (optional, requires hardware)
 * =================================================================
 *
 *   WM_BATTERY_MONITORING = 0 -> no battery cluster, no ADC reads.
 *
 *   WM_BATTERY_MONITORING = 1 -> reads battery voltage via ADC on
 *                                GPIO 4, reports through Zigbee
 *                                PowerCfg cluster. Requires the
 *                                100k+100k voltage divider hardware:
 *
 *                                Battery+ -[100k]-+-[100k]- GND
 *                                                 |
 *                                              GPIO 4
 */
#define WM_BATTERY_MONITORING    0

/* Keepalive period for deep-sleep mode: how often to wake up and
 * send a report even if no pulses arrived. Lets HA detect that the
 * device is alive. */
#define WM_KEEPALIVE_PERIOD_S    900     /* 15 minutes */

/* =================================================================
 * Pulse counting
 * =================================================================
 */

/* Number of independent water meters wired to this device. Each meter
 * gets its own GPIO, PCNT unit, RTC total, NVS key, and Zigbee
 * endpoint. Pulse weight (liters/pulse) is shared. */
#define WM_NUM_METERS            2

/* GPIOs connected to the reed/hall switches (other side to GND).
 * Both must be LP/RTC-capable on ESP32-C6 (GPIO 0..7) so they can
 * wake the chip from deep sleep. */
#define WM_PULSE_GPIO_1          2
#define WM_PULSE_GPIO_2          5

/* GPIO for battery voltage divider, only used when monitoring on. */
#define WM_BATTERY_ADC_GPIO      4

/* Liters per pulse, multiplied by 1000 (so we can express fractions
 * without floating point). 10000 = 10.000 L per pulse. */
#define WM_LITERS_PER_PULSE_X1000  10000

/* PCNT hardware glitch filter, in nanoseconds. ESP32-C6 max ~12800. */
#define WM_PCNT_GLITCH_NS        1000

/* Software debounce on top of the hardware glitch filter. Reed
 * switches need ~10-50 ms because of mechanical bounce. */
#define WM_SW_DEBOUNCE_MS        50

/* =================================================================
 * NVS persistence
 * =================================================================
 */

/* Flush the cumulative total to NVS at most this often, in seconds.
 * Avoids constant flash writes which wear NVS. */
#define WM_NVS_FLUSH_PERIOD_S    300     /* 5 minutes */

/* Flush whenever the total advanced by this many liters since the
 * last flush, even if WM_NVS_FLUSH_PERIOD_S hasn't elapsed yet. */
#define WM_NVS_FLUSH_DELTA_LITERS  10

/* =================================================================
 * Zigbee
 * =================================================================
 */

#define WM_ESP_ZB_ENDPOINT_1     10
#define WM_ESP_ZB_ENDPOINT_2     11

/* Reporting intervals for the Metering cluster. */
#define WM_REPORT_MIN_INTERVAL_S 10
#define WM_REPORT_MAX_INTERVAL_S 3600
#define WM_REPORT_DELTA          1       /* report on any change */

/* =================================================================
 * Sanity checks (don't edit below)
 * =================================================================
 */
#if !WM_POWER_USB && WM_DEEP_SLEEP
#  define WM_USE_DEEP_SLEEP      1
#else
#  define WM_USE_DEEP_SLEEP      0
#endif

#if WM_POWER_USB
#  define WM_ZB_ROLE_ROUTER      1
#  define WM_ZB_ROLE_ED          0
#else
#  define WM_ZB_ROLE_ROUTER      0
#  define WM_ZB_ROLE_ED          1
#endif
