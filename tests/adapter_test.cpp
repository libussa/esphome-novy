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
  std::cout << "ESPHome adapter tests passed\n";
}
