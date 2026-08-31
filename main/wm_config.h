#pragma once

/* =================================================================
 * ESP32-C6 Zigbee Water Meter - configuration
 * =================================================================
 *
 * Hardware: ESP32-C6 + reed switches on GPIO 2 and GPIO 5 (to GND).
 * Optional: water-leak probe on GPIO 6 (to GND).
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
#define WM_DEEP_SLEEP            1

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

/* Keepalive period.
 *   - Always-on modes: the supervisor task forces a Zigbee report on
 *     every meter (and battery, if monitored) at this cadence even
 *     when nothing changed, so HA's availability tracker stays happy.
 *   - Deep-sleep mode: the chip also wakes from sleep on this timer
 *     to send the same heartbeat reports. */
#define WM_KEEPALIVE_PERIOD_S    15    /* 1 hour */

/* =================================================================
 * Antenna selection
 * =================================================================
 *
 *   WM_ANTENNA_EXTERNAL = 0 -> use the on-module PCB/chip antenna
 *                              (default, no extra hardware needed).
 *
 *   WM_ANTENNA_EXTERNAL = 1 -> use the external U.FL/IPEX antenna
 *                              via the on-board RF switch.
 *
 * Drives the RF-switch control pin on the module. On ESP32-C6 modules
 * with a built-in antenna switch (e.g. ESP32-C6-WROOM-1 with U.FL),
 * the switch is controlled by a single GPIO: low = internal,
 * high = external. Adjust WM_ANTENNA_CTRL_GPIO to match your board.
 */
#define WM_ANTENNA_EXTERNAL      0

/* GPIO that drives the on-module RF switch VCTL line
 * (low = internal PCB antenna, high = external U.FL). */
#define WM_ANTENNA_CTRL_GPIO     14

/* GPIO that gates VDD of the on-module RF switch.
 * Active low: low = switch powered, high/Hi-Z = switch off. */
#define WM_ANTENNA_VDD_GPIO      3

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

/* =================================================================
 * Water leak sensor
 * =================================================================
 *
 *   WM_LEAK_SENSOR = 0 -> no leak endpoint, no GPIO reserved.
 *
 *   WM_LEAK_SENSOR = 1 -> a leak probe is sampled on WM_LEAK_GPIO and
 *                         published on its own Zigbee endpoint
 *                         (WM_ESP_ZB_ENDPOINT_LEAK) through the IAS
 *                         Zone cluster with ZoneType = Water Sensor.
 *                         Z2M/HA see it as a `water_leak` binary
 *                         sensor.
 *
 * Wiring for the default active-low setup (no external parts needed):
 *
 *     probe A ---- GPIO 6      (internal pull-up holds it high when dry)
 *     probe B ---- GND         (water bridges the probes -> pin reads low)
 */
#define WM_LEAK_SENSOR           1

/* GPIO the leak probe is wired to. Must be LP/RTC-capable on
 * ESP32-C6 (GPIO 0..7) to wake the chip from deep sleep. */
#define WM_LEAK_GPIO             6

/* Logic level that means "water detected".
 *
 *   0 -> active low  (bare probes to GND, internal pull-up). This is
 *        the only variant that can wake the chip from deep sleep,
 *        because esp_deep_sleep_enable_gpio_wakeup() applies a single
 *        level to the whole wake mask and the reed switches already
 *        claim wake-on-low.
 *   1 -> active high (leak module with a push-pull output, internal
 *        pull-down). Deep-sleep builds then detect leaks only on the
 *        WM_KEEPALIVE_PERIOD_S heartbeat, not instantly. */
#define WM_LEAK_ACTIVE_LEVEL     0

/* Debounce window for the probe, in milliseconds. The pin is sampled
 * repeatedly across this window and the reading only counts if every
 * sample agrees, which rejects splashes and condensation flicker. */
#define WM_LEAK_DEBOUNCE_MS      200

/* Zone ID reported in the IAS Zone enroll request / status change
 * notification. Arbitrary; only has to be stable. */
#define WM_LEAK_ZONE_ID          0x2A

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
#define WM_ESP_ZB_ENDPOINT_LEAK  12

/* Reporting intervals for the Metering cluster. */
#define WM_REPORT_MIN_INTERVAL_S 10
#define WM_REPORT_MAX_INTERVAL_S 3600
#define WM_REPORT_DELTA          1       /* report on any change */

/* Initial channel scan mask for network steering. Restricting this to
 * your coordinator's channel makes the first cold join take a few
 * seconds instead of a minute (with the full 11..26 mask). Z2M's
 * default channel is 11, ZHA's default is 15. Set to
 * ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK if you don't know it. */
#define WM_ZB_CHANNEL_MASK       (1U << 11)

/* How long deep-sleep wakes are allowed to wait for the radio before
 * giving up and going back to sleep. A successful rejoin from
 * zb_storage normally takes well under a second; the cold timeout
 * only fires on the very first boot (or after the coordinator dropped
 * us). Keep the cold timeout short to protect the battery while the
 * coordinator's permit-join window is closed. */
#define WM_ZB_JOIN_TIMEOUT_COLD_MS  10000
#define WM_ZB_JOIN_TIMEOUT_WARM_MS  3000

/* When a wake fails to join, sleep this long before trying again
 * instead of the normal WM_KEEPALIVE_PERIOD_S. Stretches battery
 * life when the network is down. */
#define WM_DEEPSLEEP_RETRY_S        300

/* =================================================================
 * Sanity checks (don't edit below)
 * =================================================================
 */
#if !WM_POWER_USB && WM_DEEP_SLEEP
#  define WM_USE_DEEP_SLEEP      1
#else
#  define WM_USE_DEEP_SLEEP      0
#endif

/* Deep sleep can only wake on the leak probe when it pulls the pin
 * low, because the reed switches already fix the wake mask level. */
#if WM_LEAK_SENSOR && !WM_LEAK_ACTIVE_LEVEL
#  define WM_LEAK_WAKES_FROM_SLEEP  1
#else
#  define WM_LEAK_WAKES_FROM_SLEEP  0
#endif

#if WM_POWER_USB
#  define WM_ZB_ROLE_ROUTER      1
#  define WM_ZB_ROLE_ED          0
#else
#  define WM_ZB_ROLE_ROUTER      0
#  define WM_ZB_ROLE_ED          1
#endif
