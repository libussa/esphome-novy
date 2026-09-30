#include "novy.h"
#include "esphome/components/api/api_server.h"
#include <cassert>
#include <iostream>

using namespace esphome;
using namespace esphome::novy;

int main() {
  NovyComponent component;
  NovyFan fan(&component);
  NovyLight light(&component);
  light::LightState state(&light);
  remote_transmitter::RemoteTransmitterComponent tx;
  sensor::Sensor power, average, age;
  binary_sensor::BinarySensor valid;
  text_sensor::TextSensor mode, status;
  component.set_transmitter(&tx);
  component.set_power_sensor(&power);
  component.set_fan(&fan);
  component.set_light(&light);
  component.set_average_sensor(&average);
  component.set_age_sensor(&age);
  component.set_valid_sensor(&valid);
  component.set_mode_sensor(&mode);
  component.set_status_sensor(&status);
  for (int speed = 0; speed <= 4; ++speed)
    for (bool on : {false, true}) {
      float watts = 2 + 30 * speed + 10 * on;
      component.add_calibration(speed, on, watts - 1, watts + 1);
    }
  // Startup LightState restoration is ignored even if it requests ON.
  state.make_call().set_state(true).perform();
  component.setup();
  assert(tx.sends == 0);
  assert(!valid.state && mode.state == "unknown");
  assert(!state.remote_values.is_on());
  state.make_call().set_state(true).perform();
  assert(tx.sends == 0);
  assert(!state.published.back());
  auto confirm = [&](float watts) {
    for (int i = 0; i < 6; ++i) {
      power.publish_state(watts);
      test_millis += 1000;
      component.loop();
    }
  };
  confirm(2);
  assert(valid.state);
  assert(tx.sends == 0); // Observing OFF did not send a toggle.
  state.make_call().set_state(true).perform();
  assert(tx.sends == 1);
  assert(!state.published.back()); // Target ON has not been confirmed.
  state.make_call().set_state(true).perform();
  assert(tx.sends == 1);
  test_millis += 1000;
  tx.completion.trigger();
  test_millis += 3000;
  component.loop();
  confirm(12);
  assert(state.remote_values.is_on());
  assert(state.published.back());
  assert(tx.sends == 1); // Publishing confirmed ON did not retransmit.
  assert(valid.state);
  state.make_call().set_state(true).perform();
  assert(tx.sends == 1); // Satisfied target is a no-op.
  // Unsupported flash is cancelled at publication and never reaches the RF.
  state.transformer = true;
  state.remote_values.set_state(false);
  state.publish_state();
  assert(!state.transformer);
  assert(state.published.back());
  assert(tx.sends == 1);
  confirm(42); // Physical remote set fan speed 1 with light on.
  assert(fan.state && fan.speed == 1);
  assert(tx.sends == 1);
  fan.request({false, {}});
  assert(tx.sends == 2);
  assert(fan.state && fan.speed == 1); // Requested OFF is not observed OFF.
  api::test_server.connected = false;
  component.loop();
  assert(!valid.state);
  state.make_call().set_state(false).perform();
  assert(tx.sends == 2);
  assert(state.published.back()); // Invalid feedback retains last observation.
  tx.completion.trigger();
  test_millis += 3000;
  component.loop();
  // A captured power press must use its measured pulses; other raw commands
  // still use the original encoder and the same controller completion lock.
  const std::vector<int32_t> captured{-12420, 370, -400, 720, -760, 370, -16220};
  component.set_power_waveform(captured);
  component.raw(Command::POWER);
  assert(tx.data == captured);
  tx.completion.trigger();
  test_millis += 3000;
  component.loop();
  component.raw(Command::LIGHT);
  assert(tx.data == waveform(Command::LIGHT, 1));

  // Standard HA fan/light calls stay usable through a same-speed light overlap.
  NovyComponent partial;
  NovyFan partial_fan(&partial);
  NovyLight partial_light(&partial);
  light::LightState partial_state(&partial_light);
  remote_transmitter::RemoteTransmitterComponent partial_tx;
  sensor::Sensor partial_power, partial_average, partial_age;
  binary_sensor::BinarySensor partial_valid, fan_valid, light_valid;
  text_sensor::TextSensor partial_mode, partial_status;
  partial.set_transmitter(&partial_tx);
  partial.set_power_sensor(&partial_power);
  partial.set_fan(&partial_fan);
  partial.set_light(&partial_light);
  partial.set_average_sensor(&partial_average);
  partial.set_age_sensor(&partial_age);
  partial.set_valid_sensor(&partial_valid);
  partial.set_speed_valid_sensor(&fan_valid);
  partial.set_light_valid_sensor(&light_valid);
  partial.set_mode_sensor(&partial_mode);
  partial.set_status_sensor(&partial_status);
  partial.set_allow_unconfirmed_light(true);
  for (int speed = 0; speed <= 4; ++speed)
    for (bool on : {false, true}) {
      float watts = 2 + 30 * speed + 10 * on;
      partial.add_calibration(speed, on, watts - 1,
                             speed == 1 && !on ? 43 : watts + 1);
    }
  api::test_server.connected = true;
  partial.setup();
  assert(partial_tx.sends == 0); // Restore never becomes a manual fallback.
  auto partial_confirm = [&](float watts) {
    for (int i = 0; i < 6; ++i) {
      partial_power.publish_state(watts);
      test_millis += 1000;
      partial.loop();
    }
  };
  partial_confirm(42);
  assert(partial_fan.state && partial_fan.speed == 1);
  assert(fan_valid.state && !light_valid.state && !partial_valid.state);
  assert(partial_mode.state == "fan_1_light_unknown");
  partial_state.make_call().set_state(true).perform();
  assert(partial_tx.sends == 1 && !partial_state.remote_values.is_on());
  partial_tx.completion.trigger();
  assert(partial_state.remote_values.is_on() && !light_valid.state);
  test_millis += 3000;
  partial.loop();
  partial_confirm(42);
  partial_state.make_call().set_state(true).perform();
  assert(partial_tx.sends == 1); // Remembered ON prevents another toggle.
  partial_fan.request({true, 2});
  assert(partial_tx.sends == 2);
  partial_tx.completion.trigger();
  test_millis += 3000;
  partial.loop();
  partial_confirm(72);
  assert(partial_fan.speed == 2 && fan_valid.state && light_valid.state);
  partial_power.publish_state(NAN);
  partial_state.make_call().set_state(false).perform();
  assert(partial_tx.sends == 2 && partial_state.remote_values.is_on());
  std::cout << "ESPHome adapter tests passed\n";
}
