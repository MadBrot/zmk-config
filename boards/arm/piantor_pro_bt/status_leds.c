#define DT_DRV_COMPAT zmk_behavior_layer_leds

#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <drivers/behavior.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#else
#include <zmk/events/split_peripheral_status_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define PIXEL_COUNT DT_PROP(STRIP_NODE, chain_length)
#define LOW_BATTERY_PERCENT 20
#define WARNING_COLOR 0x260000
#define WARNING_PULSE_MS 400
#define WARNING_PERIOD_MS 3000

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static const struct device *const power = DEVICE_DT_GET(DT_NODELABEL(led_power));
static const struct device *const fuel_gauge = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));

static atomic_t layer_color;

// Only the low-priority work queue reads and writes these values.
static struct led_rgb pixels[PIXEL_COUNT];
static uint32_t displayed_color;
static int64_t warning_started_at = -1;

static int set_strip_color(uint32_t color) {
    if (color == 0) {
        return regulator_is_enabled(power) ? regulator_disable(power) : 0;
    }

    if (!regulator_is_enabled(power)) {
        int err = regulator_enable(power);
        if (err < 0) {
            return err;
        }
    }

    struct led_rgb pixel = {.r = (color >> 16) & 0xff, .g = (color >> 8) & 0xff, .b = color & 0xff};
    for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
        pixels[i] = pixel;
    }

    return led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
}

static void update_leds(struct k_work *work) {
    if (!device_is_ready(strip) || !device_is_ready(power)) {
        return;
    }

    uint32_t color = atomic_get(&layer_color);

    if (warning_started_at >= 0) {
        int phase_ms = (k_uptime_get() - warning_started_at) % WARNING_PERIOD_MS;
        bool pulse_on = phase_ms < WARNING_PULSE_MS;
        int next_phase_ms = pulse_on ? WARNING_PULSE_MS : WARNING_PERIOD_MS;
        color = pulse_on ? WARNING_COLOR : 0;
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
                                  K_MSEC(next_phase_ms - phase_ms));
    }

    if (color == displayed_color && (color != 0 || !regulator_is_enabled(power))) {
        return;
    }

    int err = set_strip_color(color);
    if (err < 0) {
        LOG_ERR("Failed to update Piantor LEDs (%d)", err);
        return;
    }
    displayed_color = color;
}

K_WORK_DELAYABLE_DEFINE(led_work, update_leds);

static void set_layer_color(uint32_t color) {
    if ((uint32_t)atomic_set(&layer_color, color) != color) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_work, K_NO_WAIT);
    }
}

static void check_battery(struct k_work *work) {
    struct sensor_value charge;

    // ZMK pauses battery reporting when a half is idle. Sample the local gauge even then.
    if (sensor_sample_fetch_chan(fuel_gauge, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) == 0 &&
        sensor_channel_get(fuel_gauge, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &charge) == 0) {
        bool low = charge.val1 < LOW_BATTERY_PERCENT;
        if (low != (warning_started_at >= 0)) {
            warning_started_at = low ? k_uptime_get() : -1;
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &led_work, K_NO_WAIT);
        }
    }

    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
                              K_SECONDS(CONFIG_ZMK_BATTERY_REPORT_INTERVAL));
}

K_WORK_DELAYABLE_DEFINE(battery_check_work, check_battery);

static int status_leds_init(void) {
    if (device_is_ready(fuel_gauge)) {
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &battery_check_work, K_NO_WAIT);
    }
    return 0;
}

SYS_INIT(status_leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int set_layer_binding(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    set_layer_color(binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api layer_led_driver = {
    .binding_pressed = set_layer_binding,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &layer_led_driver);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static const uint32_t layer_colors[] = DT_PROP_OR(DT_DRV_INST(0), layer_colors, {0});

static void sync_layer_color(struct k_work *work) {
    zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active());
    const struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_DRV_INST(0)),
        .param1 = layer < ARRAY_SIZE(layer_colors) ? layer_colors[layer] : 0,
    };
    const struct zmk_behavior_binding_event event = {
        .layer = layer,
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .timestamp = k_uptime_get(),
    };

    zmk_behavior_invoke_binding(&binding, event, true);
}

K_WORK_DELAYABLE_DEFINE(sync_work, sync_layer_color);

static int layer_led_listener(const zmk_event_t *event) {
    if (as_zmk_layer_state_changed(event)) {
        k_work_reschedule(&sync_work, K_NO_WAIT);
    } else {
        // The initial peripheral battery read also triggers a sync after GATT discovery.
        k_work_schedule(&sync_work, K_MSEC(500));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_leds, layer_led_listener);
ZMK_SUBSCRIPTION(layer_leds, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(layer_leds, zmk_peripheral_battery_state_changed);

#else

static int layer_led_listener(const zmk_event_t *event) {
    if (!as_zmk_split_peripheral_status_changed(event)->connected) {
        set_layer_color(0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_leds, layer_led_listener);
ZMK_SUBSCRIPTION(layer_leds, zmk_split_peripheral_status_changed);

#endif
