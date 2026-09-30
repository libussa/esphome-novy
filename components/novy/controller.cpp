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
  if (calibration.size() != 10)
    return false;
  unsigned mask = 0;
  for (const auto &entry : calibration) {
    if (entry.mode.speed < 0 || entry.mode.speed > 4 || !std::isfinite(entry.min_power) ||
        !std::isfinite(entry.max_power) || entry.min_power < 0 || entry.max_power < entry.min_power)
      return false;
    unsigned bit = 1U << (entry.mode.speed * 2 + entry.mode.light);
    if (mask & bit)
      return false;
    mask |= bit;
  }
  return mask == 1023;
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
  sum_ = 0;
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
  unconfirmed_light_request_ = false;
  reset_window_(now);
  set_confidence_(false, false);
  set_status_(reason);
}

void Controller::disconnect(uint32_t now) {
  invalidate_("feedback_disconnected", now);
}

void Controller::sample(float watts, uint32_t now) {
  // Advance BEFORE adding: a late sample cannot revive an expired previous window.
  tick(now);
  if (!std::isfinite(watts) || watts < 0) {
    invalidate_("invalid_power", now);
    return;
  }
  seen_sample_ = true;
  stale_announced_ = false;
  last_sample_ = now;
  if (transmitting_ || settling_)
    return;
  sum_ += watts;
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
  const float average = count ? static_cast<float>(sum_ / count) : NAN;
  const bool skipped_window = now - window_start_ >= 2 * timing.window;
  window_start_ = now;
  sum_ = 0;
  samples_ = 0;
  if (averaged)
    averaged(average);
  if (!count || skipped_window) {
    invalidate_("missing_power_window", now);
    return;
  }
  if (!calibrated()) {
    invalidate_("uncalibrated", now);
    return;
  }
  unsigned matches = 0;
  Mode match;
  bool speed_known = true, light_known = true;
  for (const auto &entry : calibration) {
    if (average >= entry.min_power && average <= entry.max_power) {
      if (matches) {
        speed_known = speed_known && match.speed == entry.mode.speed;
        light_known = light_known && match.light == entry.mode.light;
      } else {
        match = entry.mode;
      }
      ++matches;
    }
  }
  if (!matches) {
    speed_count_ = light_count_ = 0;
    set_confidence_(false, false);
    // Fresh out-of-range readings during motor acceleration are not loss of
    // the meter. Await the deadline without retrying or advancing a fan step.
    if (active_)
      set_status_("awaiting_confirmation");
    else if (status_ != "confirmation_timeout" && status_ != "transmission_failed" &&
             status_ != "unexpected_state" && status_ != "transmitter_timeout")
      set_status_("unrecognized_power");
    return;
  }
  confirm_(speed_known, match.speed, light_known, match.light, now);
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

void Controller::confirm_(bool speed_known, int speed, bool light_known, bool light, uint32_t now) {
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
  if (!light_known) {
    light_count_ = 0;
    light_valid = false;
  } else {
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
  } else if (!speed_known || !light_known) {
    if (status_ != "confirmation_timeout" && status_ != "transmission_failed" &&
        status_ != "unexpected_state" && status_ != "transmitter_timeout")
      set_status_("ambiguous_power");
  } else if (valid_ && (status_ == "waiting_for_feedback" || status_ == "raw_sent" || status_ == "uncalibrated" ||
             status_ == "feedback_disconnected" || status_ == "feedback_stale" || status_ == "invalid_power" ||
             status_ == "unrecognized_power" || status_ == "ambiguous_power" || status_ == "missing_power_window" ||
             status_ == "light_unconfirmed" || status_ == "already_satisfied_unconfirmed")) {
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
  const bool light_fallback = kind == Target::LIGHT && allow_unconfirmed_light && speed_valid_;
  if (!calibrated() || (!known && !light_fallback)) {
    set_status_("rejected_invalid_feedback");
    return false;
  }
  const bool satisfied = kind == Target::SPEED ? target.speed == mode_.speed : target.light == mode_.light;
  if (satisfied) {
    set_status_(known ? "already_satisfied" : "already_satisfied_unconfirmed");
    return true;
  }
  target_ = target;
  target_kind_ = kind;
  unconfirmed_light_request_ = kind == Target::LIGHT && allow_unconfirmed_light;
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
  unconfirmed_light_request_ = false;
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
  if (active_ && unconfirmed_light_request_) {
    // The user explicitly chose remembered-state light control. RF completion
    // is an assumption, not reception proof; never mark it as verified.
    mode_.light = target_.light;
    active_ = false;
    unconfirmed_light_request_ = false;
    if (observed)
      observed(mode_);
    set_confidence_(false, false);
    set_status_("light_unconfirmed");
    return;
  }
  // Preserve an in-flight target's feedback failure instead of calling it a
  // successful raw press when its waveform eventually finishes.
  if (active_)
    set_status_("awaiting_confirmation");
  else if (!transmitted_target_)
    set_status_("raw_sent");
}

}  // namespace esphome::novy
