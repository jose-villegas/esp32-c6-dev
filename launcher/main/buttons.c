#include "buttons.h"
#include "button_fsm.h"

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
 * Only the short press is wanted. A long press is the PMU's own power-off and
 * is not ours to interpret. */
#define AXP2101_PKEY_SHORT   (1u << 3)

/* 50 Hz. Fast enough that a press never feels missed, slow enough that the I2C
 * read is nothing next to the rest of the frame - and deliberately decoupled
 * from the render loop, which now runs at up to 1000 fps and would hammer the
 * shared bus if it polled the PMU itself. */
#define POLL_HZ 50

static i2c_master_dev_handle_t pmu;
static bool pmu_ready;

static button_fsm_t boot_fsm;

/* PWR is an event, so there is nothing to debounce and nothing to hold - just
 * a flag waiting to be collected. */
static bool power_pressed;

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

    /* Enable the short-press interrupt, leaving the other enables alone -
     * charging and battery events are the PMU's business and stamping over
     * them would be rude. */
    uint8_t enables = 0;
    if (!pmu_read(AXP2101_REG_INTEN2, &enables)) {
        ESP_LOGW(TAG, "AXP2101 did not answer; PWR button unavailable");
        return;
    }
    if (!pmu_write(AXP2101_REG_INTEN2, enables | AXP2101_PKEY_SHORT)) {
        ESP_LOGW(TAG, "Could not enable the PWR button interrupt");
        return;
    }

    /* Clear anything already latched, so a press from before boot does not
     * arrive as the first event of the session. */
    pmu_write(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT);

    pmu_ready = true;
    ESP_LOGI(TAG, "PWR button via AXP2101, BOOT button on GPIO %d", BOOT_GPIO);
}

static bool pmu_take_short_press(void)
{
    uint8_t status = 0;
    if (!pmu_read(AXP2101_REG_INTSTS2, &status)) {
        return false;
    }
    if ((status & AXP2101_PKEY_SHORT) == 0) {
        return false;
    }

    /* Write the bit back to clear it. Only this bit: writing 0xFF would clear
     * every other latched event too, and something else may care about those. */
    pmu_write(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT);
    return true;
}

/*---------------------------------------------------------------------------
 * Polling
 *-------------------------------------------------------------------------*/

static void poll_once(void)
{
    const bool boot_down = gpio_get_level(BOOT_GPIO) == 0;   /* active low */
    const bool power_now = pmu_ready && pmu_take_short_press();
    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&lock);
    button_fsm_update(&boot_fsm, boot_down, now_us);
    if (power_now) {
        power_pressed = true;
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

    power->down     = false;   /* the PMU reports events, never a held state */
    power->pressed  = power_pressed;
    power->released = false;
    power_pressed   = false;

    portEXIT_CRITICAL(&lock);
}
