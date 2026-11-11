#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gpio_pin_test, LOG_LEVEL_INF);

namespace alc
{

  // Contact bounce on the DK's tactile switches settles well inside this.
  constexpr static uint32_t M_DEBOUNCE_MS { 30 };

  // One button driving one LED and one raw pin as a single logical channel. The
  // LED is an indicator only — the pin is the thing under test — so the two are
  // always written to the same level and never drift apart.
  struct TestChannel
  {
      const char* name;
      const char* pinName;
      struct gpio_dt_spec button;
      struct gpio_dt_spec led;
      struct gpio_dt_spec pin;
      struct gpio_callback callback;
      struct k_work_delayable debounce;
      bool asserted;
  };

  static void buttonPressed(const struct device* port, struct gpio_callback* callback, gpio_port_pins_t pins);
  static void debounceExpired(struct k_work* work);

  static TestChannel s_channels[] {
    {
        .name     = "Channel A",
        .pinName  = "P2.07",
        .button   = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
        .led      = GPIO_DT_SPEC_GET(DT_NODELABEL(led1), gpios),
        .pin      = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), channel_a_gpios),
        .callback = {},
        .debounce = {},
        .asserted = false,
    },
    {
        .name     = "Channel B",
        .pinName  = "P2.09",
        .button   = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
        .led      = GPIO_DT_SPEC_GET(DT_NODELABEL(led3), gpios),
        .pin      = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), channel_b_gpios),
        .callback = {},
        .debounce = {},
        .asserted = false,
    },
  };

  // Drives the pin first and the LED second, so a lit LED always means the write
  // to the pin under test succeeded.
  static int setChannel(TestChannel& channel, bool asserted)
  {
    int result { gpio_pin_set_dt(&channel.pin, asserted) };
    if (result < 0) {
      LOG_ERR("%s: failed to set %s: %d!", channel.name, channel.pinName, result);
      return result;
    }

    result = gpio_pin_set_dt(&channel.led, asserted);
    if (result < 0) {
      LOG_ERR("%s: failed to set LED: %d!", channel.name, result);
      return result;
    }

    channel.asserted = asserted;
    LOG_INF("%s: %s %s, LED %s", channel.name, channel.pinName, asserted ? "HIGH" : "LOW", asserted ? "on" : "off");
    return 0;
  }

  // The interrupt only starts the debounce timer. The press is not acted on here
  // because the contacts are still bouncing.
  static void buttonPressed(const struct device* port, struct gpio_callback* callback, gpio_port_pins_t pins)
  {
    ARG_UNUSED(port);
    ARG_UNUSED(pins);

    TestChannel* channel { CONTAINER_OF(callback, TestChannel, callback) };

    // Masked for the debounce window so the bounce train raises one press.
    gpio_pin_interrupt_configure_dt(&channel->button, GPIO_INT_DISABLE);
    k_work_reschedule(&channel->debounce, K_MSEC(M_DEBOUNCE_MS));
  }

  static void debounceExpired(struct k_work* work)
  {
    struct k_work_delayable* delayable { k_work_delayable_from_work(work) };
    TestChannel* channel { CONTAINER_OF(delayable, TestChannel, debounce) };

    // Re-read the button rather than trusting the edge: a bounce spike that has
    // already released is not a press.
    int pressed { gpio_pin_get_dt(&channel->button) };

    if (pressed < 0) {
      LOG_ERR("%s: failed to read button: %d!", channel->name, pressed);
    } else if (pressed == 1) {
      setChannel(*channel, !channel->asserted);
    }

    gpio_pin_interrupt_configure_dt(&channel->button, GPIO_INT_EDGE_TO_ACTIVE);
  }

  static int initChannel(TestChannel& channel)
  {
    if (!gpio_is_ready_dt(&channel.button)) {
      LOG_ERR("%s: button GPIO not ready!", channel.name);
      return -ENODEV;
    }
    if (!gpio_is_ready_dt(&channel.led)) {
      LOG_ERR("%s: LED GPIO not ready!", channel.name);
      return -ENODEV;
    }
    if (!gpio_is_ready_dt(&channel.pin)) {
      LOG_ERR("%s: %s not ready!", channel.name, channel.pinName);
      return -ENODEV;
    }

    int result { gpio_pin_configure_dt(&channel.button, GPIO_INPUT) };
    if (result < 0) {
      LOG_ERR("%s: failed to configure button: %d!", channel.name, result);
      return result;
    }

    // Both outputs start de-asserted and in step with each other.
    result = gpio_pin_configure_dt(&channel.led, GPIO_OUTPUT_INACTIVE);
    if (result < 0) {
      LOG_ERR("%s: failed to configure LED: %d!", channel.name, result);
      return result;
    }

    result = gpio_pin_configure_dt(&channel.pin, GPIO_OUTPUT_INACTIVE);
    if (result < 0) {
      LOG_ERR("%s: failed to configure %s: %d!", channel.name, channel.pinName, result);
      return result;
    }

    k_work_init_delayable(&channel.debounce, debounceExpired);
    gpio_init_callback(&channel.callback, buttonPressed, BIT(channel.button.pin));

    result = gpio_add_callback(channel.button.port, &channel.callback);
    if (result < 0) {
      LOG_ERR("%s: failed to add button callback: %d!", channel.name, result);
      return result;
    }

    result = gpio_pin_interrupt_configure_dt(&channel.button, GPIO_INT_EDGE_TO_ACTIVE);
    if (result < 0) {
      LOG_ERR("%s: failed to configure button interrupt: %d!", channel.name, result);
      return result;
    }

    LOG_INF("%s ready: button %s pin %u, LED %s pin %u, test pin %s.", channel.name, channel.button.port->name, channel.button.pin,
            channel.led.port->name, channel.led.pin, channel.pinName);
    return 0;
  }

  int runPinTest()
  {
    LOG_INF("GPIO pin test starting.");

    for (TestChannel& channel : s_channels) {
      int result { initChannel(channel) };
      if (result < 0) { return result; }
    }

    LOG_INF("Ready. Button 0 toggles P2.07 with LED1, button 1 toggles P2.09 with LED3.");

    // Everything from here runs in the button interrupt and the debounce work
    // item, so the main thread has nothing left to do.
    k_sleep(K_FOREVER);
    return 0;
  }

} // namespace alc

int main()
{
  return alc::runPinTest();
}
