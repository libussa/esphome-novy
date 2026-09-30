#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace esphome::novy {

enum class Command { LIGHT, POWER, PLUS, MINUS, NOVY };
struct Mode {
  int speed{0};
  bool light{false};
  bool operator==(const Mode &other) const { return speed == other.speed && light == other.light; }
  bool operator!=(const Mode &other) const { return !(*this == other); }
};
struct Calibration {
  Mode mode;
  float min_power;
  float max_power;
};
struct Timing {
  uint32_t window{3000};
  uint32_t stale{30000};
  uint32_t settle{3000};
  uint32_t step_timeout{30000};
};

// Pure logic: both host tests and the ESPHome adapter use this exact implementation.
class Controller {
 public:
  std::vector<Calibration> calibration;
  Timing timing;
  std::function<void(Command)> transmit;
  std::function<void(Mode)> observed;
  std::function<void(bool)> validity;
  std::function<void(bool, bool)> confidence;
  std::function<void(const std::string &)> status;
  std::function<void(float)> averaged;
  bool allow_unconfirmed_light{false};

  void start(uint32_t now);
  void sample(float watts, uint32_t now);
  void tick(uint32_t now);
  void disconnect(uint32_t now);
  bool request_speed(int speed, uint32_t now);
  bool request_light(bool light, uint32_t now);
  bool raw(Command command, uint32_t now);
  void transmission_done(uint32_t now, bool success);
  bool valid() const { return valid_; }
  bool speed_valid() const { return speed_valid_; }
  bool light_valid() const { return light_valid_; }
  std::string inferred_mode() const;
  bool has_observation() const { return has_observation_; }
  bool busy() const { return active_ || transmitting_ || pending_raw_; }
  bool calibrated() const;
  Mode mode() const { return mode_; }
  float age(uint32_t now) const { return seen_sample_ ? (now - last_sample_) / 1000.0f : NAN; }
  const std::string &last_status() const { return status_; }

 private:
  void set_status_(const char *value);
  void invalidate_(const char *reason, uint32_t now);
  void reset_window_(uint32_t now);
  enum class Target { SPEED, LIGHT };
  void set_confidence_(bool speed, bool light);
  bool request_(Mode target, Target kind, uint32_t now);
  void next_step_(uint32_t now);
  void send_(Command command, uint32_t now);
  void confirm_(bool speed_known, int speed, bool light_known, bool light, uint32_t now);

  Mode mode_{}, target_{}, before_{}, expected_{}, candidate_{};
  bool valid_{false}, has_observation_{false}, seen_sample_{false};
  bool speed_valid_{false}, light_valid_{false}, unconfirmed_light_request_{false};
  Target target_kind_{Target::SPEED};
  bool stale_announced_{false};
  bool active_{false}, transmitting_{false}, settling_{false}, pending_raw_{false};
  bool transmitted_target_{false};
  Command raw_command_{Command::LIGHT};
  uint8_t speed_count_{0}, light_count_{0};
  uint32_t last_sample_{0}, window_start_{0}, sent_at_{0}, completed_at_{0};
  double sum_{0};
  uint32_t samples_{0};
  std::string status_{"uncalibrated"};
};

// Whole button press, including ten-frame bursts and low inter-burst gaps.
std::vector<int32_t> waveform(Command command, unsigned pairing_code);
std::string mode_name(Mode mode);

}  // namespace esphome::novy
