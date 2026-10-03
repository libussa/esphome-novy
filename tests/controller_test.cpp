#include "controller.h"
#include "jsy.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <limits>

using namespace esphome::novy;

// Synthetic, deliberately separated signatures; never used as device defaults.
float fan_watts(int speed) { return 2.0f + speed * 30.0f; }
float light_watts(bool light) { return light ? 7.0f : 0.0f; }

struct Fixture {
  Controller controller;
  uint32_t now{0};
  std::vector<Command> sent;
  std::vector<Mode> observations;
  Fixture(bool calibrated = true, uint32_t start = 0) : now(start) {
    if (calibrated) {
      for (int speed = 0; speed <= 4; ++speed)
        controller.fan_calibration.push_back({speed, fan_watts(speed) - 1, fan_watts(speed) + 1});
      controller.light_threshold = 3;
    }
    controller.transmit = [this](Command command) { sent.push_back(command); };
    controller.observed = [this](Mode mode) { observations.push_back(mode); };
    controller.start(now);
  }
  void window(float fan, float light) {
    // Multiple physical samples, not repeated evaluations of a cached reading.
    for (int i = 0; i < 3; ++i) {
      controller.sample(fan, light, now);
      now += 1000;
      controller.tick(now);
    }
  }
  void window(Mode mode) { window(fan_watts(mode.speed), light_watts(mode.light)); }
  void confirm(Mode mode) { window(mode); window(mode); }
  void complete() {
    now += 1000;  // Simulated nonblocking RF duration.
    controller.transmission_done(now, true);
    now += controller.timing.settle;
    controller.tick(now);
  }
};

void test_waveform() {
  const std::string prefixes[] = {"0101", "1001", "0001", "1110", "0110", "1010", "0010", "1100", "0100", "1000"};
  for (unsigned code = 1; code <= 10; ++code) {
    for (Command command : {Command::LIGHT, Command::POWER, Command::PLUS, Command::MINUS, Command::NOVY}) {
      const char *suffix = command == Command::LIGHT ? "0111010001" : command == Command::POWER ? "0111010011" :
                           command == Command::PLUS ? "0101" : command == Command::MINUS ? "0110" : "0100";
      const auto pulses = waveform(command, code);
      unsigned frames = 0, gaps = 0;
      std::string bits;
      for (size_t i = 0; i < pulses.size();) {
        if (pulses[i] == -50000) {
          assert(frames % 10 == 0);
          ++gaps;
          ++i;
        } else if (pulses[i] == -11520) {
          assert(pulses.at(i + 1) == 320);
          assert(bits == prefixes[code - 1] + "0101" + suffix);
          bits.clear();
          ++frames;
          i += 2;
        } else {
          assert((pulses[i] == -320 && pulses.at(i + 1) == 640) ||
                 (pulses[i] == -640 && pulses.at(i + 1) == 320));
          bits += pulses[i] == -320 ? '0' : '1';
          i += 2;
        }
      }
      assert(frames == (command == Command::LIGHT ? 20 : 30));
      assert(gaps == (command == Command::LIGHT ? 1 : 2));
      assert(bits.empty());
    }
  }
  assert(waveform(Command::LIGHT, 0).empty());
  assert(waveform(Command::LIGHT, 11).empty());
}

void test_jsy_frame() {
  // Request and response data words from the JSY-MK-194T manual, section 4.4.1.
  const std::array<uint8_t, 8> request{0x01, 0x03, 0x00, 0x48, 0x00, 0x0E, 0x44, 0x18};
  assert(jsy194_request() == request);
  const uint8_t words[] = {
      0x00, 0x24, 0x1E, 0xF9, 0x00, 0x05, 0xFE, 0xA3, 0x05, 0x8D, 0x37, 0x68, 0x00, 0x00, 0x4F, 0x38,
      0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x3D, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x89,
      0x00, 0x24, 0x1E, 0xF9, 0x00, 0x05, 0xFE, 0xB9, 0x05, 0x8B, 0x67, 0x18, 0x00, 0x00, 0x4E, 0xCA,
      0x00, 0x00, 0x03, 0xE8};  // The manual omits the last register; it stays zero.
  uint8_t frame[JSY194_RESPONSE_SIZE]{0x01, 0x03, 0x38};
  std::copy(std::begin(words), std::end(words), frame + 3);
  auto seal = [](uint8_t (&value)[JSY194_RESPONSE_SIZE]) {
    const uint16_t crc = modbus_crc(value, JSY194_RESPONSE_SIZE - 2);
    value[JSY194_RESPONSE_SIZE - 2] = crc & 0xFF;
    value[JSY194_RESPONSE_SIZE - 1] = crc >> 8;
  };
  seal(frame);
  float power[2];
  assert(parse_jsy194(frame, power));
  assert(std::fabs(power[0] - 9314.084f) < 0.01f && std::fabs(power[1] - 9302.1976f) < 0.01f);
  uint8_t bad[JSY194_RESPONSE_SIZE];
  std::copy(std::begin(frame), std::end(frame), bad);
  bad[40] ^= 1;
  assert(!parse_jsy194(bad, power));  // Corrupted data.
  bad[40] ^= 1;
  bad[0] = 0x02;
  seal(bad);
  assert(!parse_jsy194(bad, power));  // Another device's well-formed reply.
}

void test_classification() {
  Fixture f;
  assert(!f.controller.valid());
  assert(f.sent.empty());
  f.window(fan_watts(2) - 1, 7);
  assert(!f.controller.valid());
  f.window(fan_watts(2) + 1, 7);
  assert(f.controller.valid());
  assert((f.controller.mode() == Mode{2, true}));
  for (int i = 0; i < 300; ++i) f.window(Mode{2, true});
  assert(f.controller.valid());
  assert(f.observations.size() == 1);
  f.window(200, 7);  // A fan transient leaves the light channel confirmed.
  assert(!f.controller.speed_valid() && f.controller.light_valid());
  assert(f.controller.last_status() == "unrecognized_power");
  assert((f.controller.mode() == Mode{2, true}));  // Last confirmed state retained.
  f.confirm({1, false});
  assert(f.controller.valid());
  assert(f.controller.last_status() == "ready");
  f.controller.fan_calibration[0].max_power = 100;
  f.window(Mode{1, false});
  assert(!f.controller.speed_valid() && f.controller.light_valid());  // Never guess between ranges.

  Fixture missing(false);
  missing.confirm({0, false});
  assert(!missing.controller.valid());
  assert(!missing.controller.request_speed(1, missing.now));
  assert(missing.sent.empty());
  assert(missing.controller.raw(Command::PLUS, missing.now));
  assert(missing.sent.size() == 1);
  Fixture partial;
  partial.controller.fan_calibration.pop_back();
  assert(!partial.controller.calibrated());
  Fixture threshold;
  threshold.controller.light_threshold = NAN;
  assert(!threshold.controller.calibrated());
}

void test_stale() {
  Fixture f;
  f.confirm({0, false});
  f.now += 3000;
  f.controller.tick(f.now);
  assert(!f.controller.valid());
  assert(f.controller.last_status() == "missing_power_window");
  f.now += 30000;
  f.controller.tick(f.now);
  assert(!f.controller.valid());
  assert(f.controller.age(f.now) >= 30);
  f.controller.sample(fan_watts(0), light_watts(false), f.now);
  assert(!f.controller.valid());
  f.confirm({0, false});
  assert(f.controller.valid());
  Fixture late;
  late.controller.sample(2, 0, 0);
  late.now = 7000;
  late.controller.tick(late.now);
  late.window(2, 0);
  assert(!late.controller.valid()); // A skipped window breaks consecutiveness.
}

void test_all_fan_targets() {
  for (int from = 0; from <= 4; ++from) {
    for (int to = 0; to <= 4; ++to) {
      for (bool light : {false, true}) {
        Fixture f;
        f.confirm({from, light});
        assert(f.controller.request_speed(to, f.now));
        if (from == to) assert(f.sent.empty());
        for (int speed = from; speed != to;) {
          assert(f.sent.back() == (to > from ? Command::PLUS : Command::MINUS));
          f.complete();
          speed += to > from ? 1 : -1;
          f.confirm({speed, light});
        }
        assert(f.sent.size() == static_cast<size_t>(std::abs(from - to)));
        assert((f.controller.mode() == Mode{to, light}));
        assert(f.controller.valid());
        assert(!f.controller.busy());
      }
    }
  }
}

void test_light_targets_and_noop() {
  for (int speed = 0; speed <= 4; ++speed) {
    for (bool light : {false, true}) {
      Fixture f;
      f.confirm({speed, light});
      assert(f.controller.request_light(light, f.now));
      assert(f.sent.empty());
      assert(f.controller.request_light(!light, f.now));
      assert(f.sent.back() == Command::LIGHT);
      assert((f.controller.mode() == Mode{speed, light}));
      assert(!f.controller.valid());
      f.complete();
      f.confirm({speed, !light});
      assert(f.controller.valid());
      assert(!f.controller.busy());
      assert(f.sent.size() == 1);
    }
  }
}

void test_failures_and_interruption() {
  Fixture missed;
  missed.confirm({1, false});
  missed.controller.request_speed(3, missed.now);
  assert(!missed.controller.request_light(true, missed.now));
  missed.complete();
  for (int i = 0; i < 10; ++i) missed.window(Mode{1, false});
  assert(!missed.controller.busy());
  assert(missed.controller.last_status() == "confirmation_timeout");
  assert(missed.sent.size() == 1);  // No automatic retry.

  Fixture changed;
  changed.confirm({1, false});
  changed.controller.request_speed(3, changed.now);
  changed.complete();
  changed.confirm({4, true});  // The fan skipped the expected speed 2.
  assert(!changed.controller.busy());
  assert(changed.controller.valid());
  assert(changed.controller.last_status() == "unexpected_state");
  assert(changed.sent.size() == 1);

  Fixture manual;
  manual.confirm({1, false});
  manual.controller.request_speed(4, manual.now);
  assert(manual.controller.raw(Command::POWER, manual.now));
  assert(!manual.controller.raw(Command::LIGHT, manual.now));
  assert(manual.sent.size() == 1);
  manual.controller.transmission_done(manual.now + 1000, true);
  assert(manual.sent.size() == 2);
  assert(manual.sent.back() == Command::POWER);
  manual.now += 1000;
  manual.complete();
  manual.confirm({0, false});
  assert(!manual.controller.busy());
  assert(manual.sent.size() == 2);

  Fixture stopped;
  stopped.confirm({2, false});
  stopped.controller.request_speed(0, stopped.now);
  stopped.complete();
  stopped.now += 2 * stopped.controller.timing.window;  // The meter stops answering.
  stopped.controller.tick(stopped.now);
  assert(!stopped.controller.busy());
  assert(stopped.controller.last_status() == "missing_power_window");
  stopped.confirm({1, false});
  assert(stopped.sent.size() == 1);

  Fixture failed;
  failed.confirm({0, false});
  failed.controller.request_light(true, failed.now);
  failed.controller.transmission_done(failed.now + 100, false);
  assert(!failed.controller.busy());
  assert(!failed.controller.valid());
  assert(failed.controller.last_status() == "transmission_failed");

  Fixture hung;
  hung.controller.raw(Command::PLUS, hung.now);
  hung.now += 30000;
  hung.controller.tick(hung.now);
  assert(hung.controller.busy());
  assert(!hung.controller.raw(Command::PLUS, hung.now));
  assert(hung.sent.size() == 1);
}

void test_independent_axes() {
  Fixture f;
  f.confirm({1, false});
  f.controller.request_speed(3, f.now);
  f.complete();
  f.confirm({2, true});  // A light change does not prevent a fan step.
  assert(f.sent.size() == 2);
  f.complete();
  f.confirm({3, true});
  assert(!f.controller.busy());
  f.window(200, 7);  // Light targets proceed through fan transients.
  assert(f.controller.request_light(false, f.now));
  assert(f.sent.back() == Command::LIGHT);
  f.complete();
  f.window(200, 0);
  f.window(200, 0);
  assert(!f.controller.busy() && f.controller.last_status() == "confirmed");
  assert(f.controller.inferred_mode() == "speed_unknown_light_off");
  assert(f.sent.size() == 3);
}

void test_motor_transients_and_fast_timing() {
  Fixture f;
  f.controller.timing = {2000, 10000, 2000, 15000};
  auto window = [&](float fan) {
    for (int i = 0; i < 2; ++i) {
      f.controller.sample(fan, 0, f.now);
      f.now += 1000;
      f.controller.tick(f.now);
    }
  };
  window(fan_watts(0)); window(fan_watts(0));
  assert(f.controller.request_speed(1, f.now));
  const uint32_t sent_at = f.now;
  f.complete();
  window(200); // Startup spike exceeds the calibrated range.
  assert(f.controller.busy() && f.sent.size() == 1);
  window(fan_watts(1)); window(fan_watts(1));
  assert(f.controller.speed_valid() && !f.controller.busy());
  assert(f.now - sent_at == 9000); // RF + settle + transient + two windows.
  assert(f.controller.last_status() == "confirmed");

  assert(f.controller.request_speed(2, f.now));
  f.complete();
  for (int i = 0; i < 8; ++i) window(200);
  assert(!f.controller.busy() && f.controller.last_status() == "confirmation_timeout");
  assert(f.sent.size() == 2); // No retry and no next step through unknown power.
}

void test_wrap_and_settling() {
  Fixture f(true, std::numeric_limits<uint32_t>::max() - 3500);
  f.confirm({0, false});
  assert(f.controller.valid());
  f.controller.request_speed(1, f.now);
  for (int i = 0; i < 20; ++i) f.controller.sample(fan_watts(1), 0, f.now + i);
  assert((f.controller.mode() == Mode{0, false}));
  f.complete();
  f.window(Mode{1, false});
  assert(!f.controller.valid()); // In-flight readings cannot supply confirmation.
  f.window(Mode{1, false});
  assert(f.controller.valid());
  assert(f.sent.size() == 1);
}

int main() {
  test_waveform();
  test_jsy_frame();
  test_classification();
  test_stale();
  test_all_fan_targets();
  test_light_targets_and_noop();
  test_failures_and_interruption();
  test_wrap_and_settling();
  test_independent_axes();
  test_motor_transients_and_fast_timing();
  std::cout << "Controller, JSY and RF waveform tests passed\n";
}
