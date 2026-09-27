#include "controller.h"

#include <cassert>
#include <iostream>
#include <limits>

using namespace esphome::novy;

// Synthetic, deliberately separated signatures; never used as device defaults.
float watts(Mode mode) { return 2.0f + mode.speed * 30.0f + (mode.light ? 10.0f : 0); }

struct Fixture {
  Controller controller;
  uint32_t now{0};
  std::vector<Command> sent;
  std::vector<Mode> observations;
  Fixture(bool calibrated = true, uint32_t start = 0) : now(start) {
    if (calibrated) {
      for (int speed = 0; speed <= 4; ++speed)
        for (bool light : {false, true}) {
          Mode mode{speed, light};
          controller.calibration.push_back({mode, watts(mode) - 1, watts(mode) + 1});
        }
    }
    controller.transmit = [this](Command command) { sent.push_back(command); };
    controller.observed = [this](Mode mode) { observations.push_back(mode); };
    controller.start(now);
  }
  void window(float value) {
    // Multiple physical samples, not repeated evaluations of a cached reading.
    for (int i = 0; i < 3; ++i) {
      controller.sample(value, now);
      now += 1000;
      controller.tick(now);
    }
  }
  void confirm(Mode mode) { window(watts(mode)); window(watts(mode)); }
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

void test_classification() {
  Fixture f;
  assert(!f.controller.valid());
  assert(f.sent.empty());
  f.window(watts({2, true}) - 1);
  assert(!f.controller.valid());
  f.window(watts({2, true}) + 1);
  assert(f.controller.valid());
  assert((f.controller.mode() == Mode{2, true}));
  for (int i = 0; i < 300; ++i) f.window(watts({2, true}));
  assert(f.controller.valid());
  assert(f.observations.size() == 1);
  f.window(200);
  assert(!f.controller.valid());
  assert((f.controller.mode() == Mode{2, true}));  // Last confirmed state retained.
  f.confirm({1, false});
  assert(f.controller.valid());
  f.controller.calibration[0].min_power = 0;
  f.controller.calibration[0].max_power = 100;
  f.window(watts({1, false}));
  assert(!f.controller.valid());
  assert(f.controller.last_status() == "ambiguous_power");

  Fixture missing(false);
  missing.confirm({0, false});
  assert(!missing.controller.valid());
  assert(!missing.controller.request_speed(1, missing.now));
  assert(missing.sent.empty());
  assert(missing.controller.raw(Command::PLUS, missing.now));
  assert(missing.sent.size() == 1);
  Fixture partial;
  partial.controller.calibration.pop_back();
  assert(!partial.controller.calibrated());
}

void test_stale_and_invalid() {
  for (float invalid : {NAN, INFINITY, -1.0f}) {
    Fixture f;
    f.confirm({1, true});
    f.controller.sample(invalid, f.now);
    assert(!f.controller.valid());
    assert(!f.controller.request_light(false, f.now));
    assert(f.sent.empty());
    f.confirm({1, true});
    assert(f.controller.valid());
  }
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
  f.controller.sample(watts({0, false}), f.now);
  assert(!f.controller.valid());
  f.confirm({0, false});
  assert(f.controller.valid());
  f.controller.disconnect(f.now);
  assert(!f.controller.valid());
  assert(std::isfinite(f.controller.age(f.now)));
  f.confirm({0, false});
  assert(f.controller.valid());
  Fixture late;
  late.controller.sample(2, 0);
  late.now = 7000;
  late.controller.tick(late.now);
  late.window(2);
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
  for (int i = 0; i < 10; ++i) missed.window(watts({1, false}));
  assert(!missed.controller.busy());
  assert(missed.controller.last_status() == "confirmation_timeout");
  assert(missed.sent.size() == 1);  // No automatic retry.

  Fixture changed;
  changed.confirm({1, false});
  changed.controller.request_speed(3, changed.now);
  changed.complete();
  changed.confirm({2, true});  // Someone changed the light while fan step was pending.
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
  stopped.controller.sample(NAN, stopped.now);
  assert(!stopped.controller.busy());
  stopped.confirm({1, false});
  assert(stopped.sent.size() == 1);

  Fixture failed;
  failed.confirm({0, false});
  failed.controller.request_light(true, failed.now);
  failed.controller.transmission_done(failed.now + 100, false);
  assert(!failed.controller.busy());
  assert(!failed.controller.valid());
  assert(failed.controller.last_status() == "transmission_failed");

  Fixture unplugged;
  unplugged.confirm({1, false});
  unplugged.controller.request_speed(2, unplugged.now);
  unplugged.controller.disconnect(unplugged.now);
  unplugged.controller.transmission_done(unplugged.now + 1000, true);
  assert(unplugged.controller.last_status() == "feedback_disconnected");
  assert(!unplugged.controller.busy());
  assert(!unplugged.controller.valid());

  Fixture hung;
  hung.controller.raw(Command::PLUS, hung.now);
  hung.now += 30000;
  hung.controller.tick(hung.now);
  assert(hung.controller.busy());
  assert(!hung.controller.raw(Command::PLUS, hung.now));
  assert(hung.sent.size() == 1);
}

void test_wrap_and_settling() {
  Fixture f(true, std::numeric_limits<uint32_t>::max() - 3500);
  f.confirm({0, false});
  assert(f.controller.valid());
  f.controller.request_speed(1, f.now);
  for (int i = 0; i < 20; ++i) f.controller.sample(watts({1, false}), f.now + i);
  assert((f.controller.mode() == Mode{0, false}));
  f.complete();
  f.window(watts({1, false}));
  assert(!f.controller.valid()); // In-flight readings cannot supply confirmation.
  f.window(watts({1, false}));
  assert(f.controller.valid());
  assert(f.sent.size() == 1);
}

int main() {
  test_waveform();
  test_classification();
  test_stale_and_invalid();
  test_all_fan_targets();
  test_light_targets_and_noop();
  test_failures_and_interruption();
  test_wrap_and_settling();
  std::cout << "Controller and RF waveform tests passed\n";
}
