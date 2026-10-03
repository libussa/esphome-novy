#include "controller.h"

namespace esphome::novy {

std::string mode_name(Mode mode) {
  return std::string(mode.speed == 0 ? "off" : mode.speed == 4 ? "boost" : "fan_" + std::to_string(mode.speed)) +
         (mode.light ? "_light" : "");
}

std::vector<int32_t> waveform(Command command, unsigned pairing_code) {
  static const char *const codes[] = {"0101", "1001", "0001", "1110", "0110",
                                    "1010", "0010", "1100", "0100", "1000"};
  if (pairing_code < 1 || pairing_code > 10)
    return {};
  const char *suffix = nullptr;
  switch (command) {
    case Command::LIGHT: suffix = "0111010001"; break;
    case Command::POWER: suffix = "0111010011"; break;
    case Command::PLUS: suffix = "0101"; break;
    case Command::MINUS: suffix = "0110"; break;
    case Command::NOVY: suffix = "0100"; break;
  }
  if (suffix == nullptr)
    return {};
  const std::string bits = std::string(codes[pairing_code - 1]) + "0101" + suffix;
  const unsigned bursts = command == Command::LIGHT ? 2 : 3;
  std::vector<int32_t> result;
  result.reserve(bursts * (10 * (bits.size() + 1) * 2 + 1));
  for (unsigned burst = 0; burst < bursts; ++burst) {
    for (unsigned frame = 0; frame < 10; ++frame) {
      for (char bit : bits) {
        result.push_back(bit == '0' ? -320 : -640);
        result.push_back(bit == '0' ? 640 : 320);
      }
      result.push_back(-11520);
      result.push_back(320);
    }
    if (burst + 1 < bursts)
      result.push_back(-50000);
  }
  return result;
}

bool Controller::calibrated() const {
  if (fan_calibration.size() != 5 || !std::isfinite(light_threshold) || light_threshold <= 0)
    return false;
  unsigned mask = 0;
  for (const auto &range : fan_calibration) {
    if (range.speed < 0 || range.speed > 4 || !std::isfinite(range.min_power) ||
        !std::isfinite(range.max_power) || range.min_power < 0 || range.max_power < range.min_power)
      return false;
    mask |= 1U << range.speed;
  }
  return mask == 31;
}

void Controller::set_status_(const char *value) {
  if (status_ == value)
    return;
  status_ = value;
  if (status)
    status(status_);
}

void Controller::reset_window_(uint32_t now) {
  window_start_ = now;
  fan_sum_ = light_sum_ = 0;
  samples_ = 0;
  speed_count_ = light_count_ = 0;
}

void Controller::start(uint32_t now) {
  reset_window_(now);
  set_confidence_(false, false);
  status_.clear();  // Publish the initial status even when it is uncalibrated.
  set_status_(calibrated() ? "waiting_for_feedback" : "uncalibrated");
}

void Controller::invalidate_(const char *reason, uint32_t now) {
  active_ = false;
  reset_window_(now);
  set_confidence_(false, false);
  set_status_(reason);
}

void Controller::sample(float fan_watts, float light_watts, uint32_t now) {
  // Advance BEFORE adding: a late sample cannot revive an expired previous window.
  tick(now);
  seen_sample_ = true;
  stale_announced_ = false;
  last_sample_ = now;
  if (transmitting_ || settling_)
    return;
  fan_sum_ += fan_watts;
  light_sum_ += light_watts;
  ++samples_;
}

void Controller::tick(uint32_t now) {
  // Keep the RF lock if completion is missing; never start a second transmission.
  if (transmitting_ && now - sent_at_ >= timing.step_timeout) {
    pending_raw_ = false;
    if (status_ != "transmitter_timeout")
      invalidate_("transmitter_timeout", now);
    return;
  }
  if (seen_sample_ && !stale_announced_ && now - last_sample_ >= timing.stale) {
    stale_announced_ = true;
    invalidate_("feedback_stale", now);
  }
  if (transmitting_)
    return;
  if (active_ && now - completed_at_ >= timing.step_timeout) {
    invalidate_("confirmation_timeout", now);
    return;
  }
  if (settling_) {
    if (now - completed_at_ < timing.settle)
      return;
    settling_ = false;
    reset_window_(now);
  }
  if (now - window_start_ < timing.window)
    return;
  const auto count = samples_;
  const float fan = count ? static_cast<float>(fan_sum_ / count) : NAN;
  const float light = count ? static_cast<float>(light_sum_ / count) : NAN;
  const bool skipped_window = now - window_start_ >= 2 * timing.window;
  window_start_ = now;
  fan_sum_ = light_sum_ = 0;
  samples_ = 0;
  if (averaged)
    averaged(fan, light);
  if (!count || skipped_window) {
    invalidate_("missing_power_window", now);
    return;
  }
  if (!calibrated()) {
    invalidate_("uncalibrated", now);
    return;
  }
  // The light channel is a plain threshold. The fan channel must fall in
  // exactly one range; no nearest-range guess is made.
  unsigned matches = 0;
  int speed = 0;
  for (const auto &range : fan_calibration) {
    if (fan >= range.min_power && fan <= range.max_power) {
      speed = range.speed;
      ++matches;
    }
  }
  confirm_(matches == 1, speed, light >= light_threshold, now);
}

void Controller::set_confidence_(bool speed, bool light) {
  speed_valid_ = speed;
  light_valid_ = light;
  valid_ = speed && light;
  if (validity)
    validity(valid_);
  if (confidence)
    confidence(speed, light);
}

std::string Controller::inferred_mode() const {
  if (speed_valid_)
    return mode_name({mode_.speed, false}) +
           (light_valid_ ? (mode_.light ? "_light" : "") : "_light_unknown");
  if (light_valid_)
    return mode_.light ? "speed_unknown_light_on" : "speed_unknown_light_off";
  return "unknown";
}

void Controller::confirm_(bool speed_known, int speed, bool light, uint32_t now) {
  const Mode previous = mode_;
  bool speed_valid = speed_valid_, light_valid = light_valid_;
  if (!speed_known) {
    speed_count_ = 0;
    speed_valid = false;
  } else {
    if (speed != mode_.speed)
      speed_valid = false;
    if (!speed_count_ || speed != candidate_.speed) {
      candidate_.speed = speed;
      speed_count_ = 1;
    } else {
      speed_count_ = 2;
      mode_.speed = speed;
      speed_valid = true;
    }
  }
  if (light != mode_.light)
    light_valid = false;
  if (!light_count_ || light != candidate_.light) {
    candidate_.light = light;
    light_count_ = 1;
  } else {
    light_count_ = 2;
    mode_.light = light;
    light_valid = true;
  }
  if ((speed_valid || light_valid) && (!has_observation_ || mode_ != previous)) {
    has_observation_ = true;
    if (observed)
      observed(mode_);
  }
  set_confidence_(speed_valid, light_valid);
  if (active_) {
    if (target_kind_ == Target::SPEED ? !speed_valid_ : !light_valid_)
      return;
    const bool unchanged = target_kind_ == Target::SPEED ? mode_.speed == before_.speed : mode_.light == before_.light;
    if (unchanged)
      return;
    const bool expected = target_kind_ == Target::SPEED ? mode_.speed == expected_.speed : mode_.light == expected_.light;
    if (!expected) {
      active_ = false;
      set_status_("unexpected_state");
      return;
    }
    const bool reached = target_kind_ == Target::SPEED ? mode_.speed == target_.speed : mode_.light == target_.light;
    if (reached) {
      active_ = false;
      set_status_("confirmed");
    } else {
      next_step_(now);
    }
  } else if (!speed_known) {
    // Fresh out-of-range fan readings, e.g. motor acceleration, are not loss
    // of the meter; the light axis stays usable. An active fan step instead
    // awaits its deadline above, without retrying.
    if (status_ != "confirmation_timeout" && status_ != "transmission_failed" &&
        status_ != "unexpected_state" && status_ != "transmitter_timeout")
      set_status_("unrecognized_power");
  } else if (valid_ && (status_ == "waiting_for_feedback" || status_ == "raw_sent" || status_ == "uncalibrated" ||
             status_ == "feedback_stale" || status_ == "unrecognized_power" ||
             status_ == "missing_power_window")) {
    set_status_("ready");
  }
}

bool Controller::request_(Mode target, Target kind, uint32_t now) {
  tick(now);
  if (busy()) {
    set_status_("rejected_busy");
    return false;
  }
  const bool known = kind == Target::SPEED ? speed_valid_ : light_valid_;
  if (!calibrated() || !known) {
    set_status_("rejected_invalid_feedback");
    return false;
  }
  const bool satisfied = kind == Target::SPEED ? target.speed == mode_.speed : target.light == mode_.light;
  if (satisfied) {
    set_status_("already_satisfied");
    return true;
  }
  target_ = target;
  target_kind_ = kind;
  active_ = true;
  next_step_(now);
  return true;
}

bool Controller::request_speed(int speed, uint32_t now) {
  tick(now);
  if (speed < 0 || speed > 4) {
    set_status_("rejected_speed");
    return false;
  }
  return request_({speed, mode_.light}, Target::SPEED, now);
}

bool Controller::request_light(bool light, uint32_t now) {
  tick(now);
  return request_({mode_.speed, light}, Target::LIGHT, now);
}

void Controller::next_step_(uint32_t now) {
  before_ = mode_;
  expected_ = mode_;
  Command command;
  if (target_kind_ == Target::SPEED) {
    bool up = target_.speed > mode_.speed;
    expected_.speed += up ? 1 : -1;
    command = up ? Command::PLUS : Command::MINUS;
  } else {
    expected_.light = !mode_.light;
    command = Command::LIGHT;
  }
  send_(command, now);
}

void Controller::send_(Command command, uint32_t now) {
  set_confidence_(false, false);
  reset_window_(now);
  transmitting_ = true;
  transmitted_target_ = active_;
  settling_ = false;
  sent_at_ = now;
  set_status_(active_ ? "transmitting_target" : "transmitting_raw");
  if (transmit)
    transmit(command);
}

bool Controller::raw(Command command, uint32_t now) {
  if (pending_raw_ || (transmitting_ && now - sent_at_ >= timing.step_timeout)) {
    set_status_("rejected_busy");
    return false;
  }
  active_ = false;
  if (transmitting_) {
    // One bounded pending manual press. Finish the in-flight waveform first.
    pending_raw_ = true;
    raw_command_ = command;
    set_status_("raw_queued");
  } else {
    send_(command, now);
  }
  return true;
}

void Controller::transmission_done(uint32_t now, bool success) {
  if (!transmitting_)
    return;
  const bool late = now - sent_at_ >= timing.step_timeout;
  transmitting_ = false;
  if (!success || late) {
    pending_raw_ = false;
    invalidate_("transmission_failed", now);
    return;
  }
  if (pending_raw_) {
    pending_raw_ = false;
    send_(raw_command_, now);
    return;
  }
  completed_at_ = now;
  settling_ = true;
  reset_window_(now);
  // Preserve an in-flight target's feedback failure instead of calling it a
  // successful raw press when its waveform eventually finishes.
  if (active_)
    set_status_("awaiting_confirmation");
  else if (!transmitted_target_)
    set_status_("raw_sent");
}

}  // namespace esphome::novy
