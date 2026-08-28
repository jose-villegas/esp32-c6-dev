#include "input/buttons.h"
#include "input/button_fsm.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

/* The BOOT button. Standard on every ESP32-C6 board: GPIO 9, pulled up, and
 * pulled to ground when pressed - so a LOW level means down. */
#define BOOT_GPIO GPIO_NUM_9

/* AXP2101 power-management chip. The PWR button is wired to it, not to the
 * SoC, so the only way to see a press is to ask the PMU over I2C.
 *
 * Registers from the X-Powers AXP2101 datasheet. Interrupts are arranged as
 * three enable registers and three status registers; the power-key events live
 * in the second of each. A status bit is cleared by writing a one back to it,
 * which is worth noticing - the obvious "write zero to clear" leaves the flag
 * set and the button appears stuck. */
#define AXP2101_ADDR         0x34
#define AXP2101_I2C_HZ       400000
#define AXP2101_TIMEOUT_MS   100

#define AXP2101_REG_INTEN2   0x41
#define AXP2101_REG_INTSTS2  0x49

/* Within INTEN2 / INTSTS2:
 *   bit 0  power key rising edge
 *   bit 1  power key falling edge
 *   bit 2  power key long press
 *   bit 3  power key short press
 * Short and long press are both enabled; rising/falling edge are not (see
 * buttons.h - there is no `down` level to build from them). Long press fires
 * at its own threshold (REG 0x27 bits 5:4, irqlevel - 1/1.5/2/2.5 s),
 * independent of the PMU's own power-off, a different threshold again (REG
 * 0x27 bits 3:2, offlevel - 4/6/8/10 s) gated by its own enable bit (REG
 * 0x22 bit 1, btn_pwroff_en) that firmware could clear - this bit does not
 * touch it, so both the long-press interrupt below and the PMU's own
 * eventual power-off can fire from the same sustained hold. */
#define AXP2101_PKEY_SHORT   (1u << 3)
#define AXP2101_PKEY_LONG    (1u << 2)

/* REG 0x27, IRQLEVEL/OFFLEVEL/ONLEVEL setting - three independent thresholds
 * packed into one register: bits 5:4 the long-press IRQ above, bits 3:2 the
 * PMU's own power-off, bits 1:0 power-on. Read-only here; see pmu_init(). */
#define AXP2101_REG_LEVELS   0x27

/* REG 0x22, bit 1 btn_pwroff_en - enables PWRON held past OFFLEVEL as a
 * power-off source at all - and bit 0 btn_pwroff_mode - power-off (0) vs
 * restart (1) when it fires. Both default from EFUSE/POR, so what a given
 * board actually boots with isn't knowable from the datasheet; only logged
 * here, never written. */
#define AXP2101_REG_PWROFF   0x22

/* 50 Hz. Fast enough that a press never feels missed, slow enough that the I2C
 * read is nothing next to the rest of the frame - and deliberately decoupled
 * from the render loop, which now runs at up to 1000 fps and would hammer the
 * shared bus if it polled the PMU itself. */
#define POLL_HZ 50

static i2c_master_dev_handle_t pmu;
static bool pmu_ready;

static button_fsm_t boot_fsm;

/* PWR is an event, so there is nothing to debounce - just flags waiting to be
 * collected. Two of them, one per interrupt bit enabled below: the PMU
 * itself is what decides "held", by timing the press against its own
 * irqlevel threshold, so there is no local hold-timer state to keep the way
 * BOOT's button_fsm_t needs one. */
static bool power_pressed;
static bool power_held;

/* Shared between the polling task and the render loop. The chip is
 * single-core, so a spinlock-guarded critical section is both correct and
 * essentially free here. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

/*---------------------------------------------------------------------------
 * The PMU
 *-------------------------------------------------------------------------*/

static bool pmu_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(pmu, buf, sizeof(buf), AXP2101_TIMEOUT_MS) == ESP_OK;
}

static bool pmu_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(pmu, &reg, 1, value, 1,
                                       AXP2101_TIMEOUT_MS) == ESP_OK;
}

static void pmu_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "No I2C bus - the PWR button will not report");
        return;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = AXP2101_I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &config, &pmu) != ESP_OK) {
        ESP_LOGW(TAG, "Could not add the AXP2101 to the bus");
        return;
    }

    /* Enable the short-press and long-press interrupts, leaving the other
     * enables alone - charging and battery events are the PMU's business and
     * stamping over them would be rude. */
    uint8_t enables = 0;
    if (!pmu_read(AXP2101_REG_INTEN2, &enables)) {
        ESP_LOGW(TAG, "AXP2101 did not answer; PWR button unavailable");
        return;
    }
    if (!pmu_write(AXP2101_REG_INTEN2,
                   enables | AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG)) {
        ESP_LOGW(TAG, "Could not enable the PWR button interrupts");
        return;
    }

    /* Clear anything already latched, so a press from before boot does not
     * arrive as the first event of the session. */
    pmu_write(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);

    pmu_ready = true;
    ESP_LOGI(TAG, "PWR button via AXP2101, BOOT button on GPIO %d", BOOT_GPIO);

    /* Log what this board's PMU actually boots with. 0x22 and 0x27 default
     * from EFUSE/POR, so this is not something the datasheet can answer and
     * the only way to know is to ask the chip, once, here. Observation only -
     * neither register is written. */
    uint8_t levels = 0, pwroff = 0;
    if (pmu_read(AXP2101_REG_LEVELS, &levels) && pmu_read(AXP2101_REG_PWROFF, &pwroff)) {
        static const char *irq_level[4] = { "1s", "1.5s", "2s", "2.5s" };
        static const char *off_level[4] = { "4s", "6s", "8s", "10s" };
        static const char *on_level[4]  = { "128ms", "512ms", "1s", "2s" };
        const uint8_t irqlevel     = (levels >> 4) & 0x3;
        const uint8_t offlevel     = (levels >> 2) & 0x3;
        const uint8_t onlevel      = levels & 0x3;
        const bool    pwroff_en    = (pwroff & (1u << 1)) != 0;
        const bool    pwroff_mode  = (pwroff & (1u << 0)) != 0;
        ESP_LOGI(TAG, "AXP2101 PWR thresholds: irqlevel %s, offlevel %s, onlevel %s, "
                       "pwroff_en %d, pwroff_mode %s",
                 irq_level[irqlevel], off_level[offlevel], on_level[onlevel],
                 pwroff_en, pwroff_mode ? "restart" : "power-off");
    } else {
        ESP_LOGW(TAG, "Could not read AXP2101 0x22/0x27 - PWR thresholds unknown");
    }
}

/* One I2C read serves both events rather than one each - poll_once() runs at
 * POLL_HZ regardless of whether anything happened, so halving its bus traffic
 * here is free. */
static void pmu_take_events(bool *short_press, bool *long_press)
{
    *short_press = false;
    *long_press  = false;

    uint8_t status = 0;
    if (!pmu_read(AXP2101_REG_INTSTS2, &status)) {
        return;
    }

    const uint8_t fired = status & (AXP2101_PKEY_SHORT | AXP2101_PKEY_LONG);
    if (fired == 0) {
        return;
    }

    /* Write the fired bits back to clear them. Only these two: writing 0xFF
     * would clear every other latched event too, and something else may care
     * about those. */
    pmu_write(AXP2101_REG_INTSTS2, fired);

    *short_press = (status & AXP2101_PKEY_SHORT) != 0;
    *long_press  = (status & AXP2101_PKEY_LONG) != 0;
}

/*---------------------------------------------------------------------------
 * Polling
 *-------------------------------------------------------------------------*/

static void poll_once(void)
{
    const bool boot_down = gpio_get_level(BOOT_GPIO) == 0;   /* active low */
    bool short_press = false, long_press = false;
    if (pmu_ready) {
        pmu_take_events(&short_press, &long_press);
    }
    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&lock);
    button_fsm_update(&boot_fsm, boot_down, now_us);
    if (short_press) {
        power_pressed = true;
    }
    if (long_press) {
        power_held = true;
    }
    portEXIT_CRITICAL(&lock);
}

static void buttons_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / POLL_HZ);

    while (1) {
        poll_once();
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);
    }
}

void buttons_start(void)
{
    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);

    button_fsm_reset(&boot_fsm);
    pmu_init();

    xTaskCreate(buttons_task, "buttons", 3072, NULL, 5, NULL);
}

void buttons_read(button_t *boot, button_t *power)
{
    portENTER_CRITICAL(&lock);

    boot->down     = button_fsm_is_down(&boot_fsm);
    boot->pressed  = button_fsm_take_pressed(&boot_fsm);
    boot->released = button_fsm_take_released(&boot_fsm);
    boot->held     = button_fsm_take_held(&boot_fsm);

    /* Not a level because the PMU's edge interrupts are not enabled, not
     * because it cannot report one - see buttons.h. */
    power->down     = false;
    power->pressed  = power_pressed;
    power->released = false;
    power->held     = power_held;
    power_pressed   = false;
    power_held      = false;

    portEXIT_CRITICAL(&lock);
}
