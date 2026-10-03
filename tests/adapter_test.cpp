#include "novy.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

using namespace esphome;
using namespace esphome::novy;

// A well-formed JSY-MK-194G reply carrying both channels' active power.
std::vector<uint8_t> reply(float channel1, float channel2) {
  std::vector<uint8_t> frame(JSY194_RESPONSE_SIZE);
  frame[0] = 0x01;
  frame[1] = 0x03;
  frame[2] = JSY194_RESPONSE_SIZE - 5;
  for (auto [address, watts] : {std::pair{0x4Au, channel1}, std::pair{0x52u, channel2}}) {
    const auto value = static_cast<uint32_t>(std::lround(watts * 10000));
    for (unsigned i = 0; i < 4; ++i)
      frame[3 + (address - 0x48) * 4 + i] = value >> (24 - 8 * i);
  }
  const uint16_t crc = modbus_crc(frame.data(), JSY194_RESPONSE_SIZE - 2);
  frame[JSY194_RESPONSE_SIZE - 2] = crc & 0xFF;
  frame[JSY194_RESPONSE_SIZE - 1] = crc >> 8;
  return frame;
}

int main() {
  NovyComponent component;
  NovyFan fan(&component);
  NovyLight light(&component);
  light::LightState state(&light);
  remote_transmitter::RemoteTransmitterComponent tx;
  sensor::Sensor fan_power, light_power, age;
  binary_sensor::BinarySensor valid;
  text_sensor::TextSensor mode, status;
  component.set_transmitter(&tx);
  component.set_channels(2, 1);
  component.set_fan(&fan);
  component.set_light(&light);
  component.set_fan_power_sensor(&fan_power);
  component.set_light_power_sensor(&light_power);
  component.set_age_sensor(&age);
  component.set_valid_sensor(&valid);
  component.set_mode_sensor(&mode);
  component.set_status_sensor(&status);
  for (int speed = 0; speed <= 4; ++speed)
    component.add_fan_range(speed, 1 + 30 * speed, 3 + 30 * speed);
  component.set_light_threshold(3);
  // Startup LightState restoration is ignored even if it requests ON.
  state.make_call().set_state(true).perform();
  component.setup();
  assert(tx.sends == 0);
  assert(!valid.state && mode.state == "unknown");
  assert(!state.remote_values.is_on());
  state.make_call().set_state(true).perform();
  assert(tx.sends == 0);
  assert(!state.published.back());
  // Each poll parses the previous reply, then requests the next one.
  auto meter = [&](const std::vector<uint8_t> &bytes, int polls) {
    for (int i = 0; i < polls; ++i) {
      test_millis += 500;
      component.loop();
      component.rx.insert(component.rx.end(), bytes.begin(), bytes.end());
    }
  };
  auto confirm = [&](float fan_watts, float light_watts) { meter(reply(light_watts, fan_watts), 18); };
  confirm(2, 0);
  const auto request = jsy194_request();
  assert(component.written.size() == 18);
  assert(component.written.back() == std::vector<uint8_t>(request.begin(), request.end()));
  assert(valid.state);
  assert(fan_power.state == 2 && light_power.state == 0);  // Channel 2 is the fan.
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
  confirm(2, 7);
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
  confirm(32, 7); // Physical remote set fan speed 1 with light on.
  assert(fan.state && fan.speed == 1);
  assert(tx.sends == 1);
  fan.request({false, {}});
  assert(tx.sends == 2);
  assert(fan.state && fan.speed == 1); // Requested OFF is not observed OFF.
  tx.completion.trigger();
  test_millis += 3000;
  component.loop();
  auto corrupted = reply(7, 32);
  corrupted[10] ^= 1;
  meter(corrupted, 14);  // The meter answers, but every reply is rejected.
  assert(!valid.state && status.state == "missing_power_window");
  state.make_call().set_state(false).perform();
  assert(tx.sends == 2);
  assert(state.published.back()); // Invalid feedback retains last observation.
  assert(fan.state && fan.speed == 1);
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
  std::cout << "ESPHome adapter tests passed\n";
}
