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
  status_ = value;
  if (status)
    status(status_);
}

void Controller::reset_window_(uint32_t now) {
  window_start_ = now;
  sum_ = 0;
  samples_ = 0;
  candidate_count_ = 0;
}

void Controller::start(uint32_t now) {
  reset_window_(now);
  if (validity)
    validity(false);
  set_status_(calibrated() ? "waiting_for_feedback" : "uncalibrated");
}

void Controller::invalidate_(const char *reason, uint32_t now) {
  valid_ = false;
  active_ = false;
  reset_window_(now);
  if (validity)
    validity(false);
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
  for (const auto &entry : calibration) {
    if (average >= entry.min_power && average <= entry.max_power) {
      ++matches;
      match = entry.mode;
    }
  }
  if (matches != 1) {
    invalidate_(matches == 0 ? "unrecognized_power" : "ambiguous_power", now);
    return;
  }
  // A different candidate cannot authorize actions based on the old observation.
  if (valid_ && match != mode_) {
    valid_ = false;
    if (validity)
      validity(false);
  }
  if (!candidate_count_ || match != candidate_) {
    candidate_ = match;
    candidate_count_ = 1;
    return;
  }
  candidate_count_ = 2;  // Saturates; long stable runs cannot overflow.
  confirm_(match, now);
}

void Controller::confirm_(Mode mode, uint32_t now) {
  const bool changed = !has_observation_ || mode != mode_;
  mode_ = mode;
  has_observation_ = true;
  valid_ = true;
  if (changed && observed)
    observed(mode);
  if (validity)
    validity(true);
  if (active_) {
    if (mode == before_)
      return;  // Missed command: await deadline, do not resend.
    if (mode != expected_) {
      active_ = false;
      set_status_("unexpected_state");
      return;
    }
    if (mode == target_) {
      active_ = false;
      set_status_("confirmed");
    } else {
      next_step_(now);
    }
  } else if (status_ == "waiting_for_feedback" || status_ == "raw_sent" || status_ == "uncalibrated" ||
             status_ == "feedback_disconnected" || status_ == "feedback_stale" || status_ == "invalid_power" ||
             status_ == "unrecognized_power" || status_ == "ambiguous_power" || status_ == "missing_power_window") {
    set_status_("ready");
  }
}

bool Controller::request_(Mode target, uint32_t now) {
  tick(now);
  if (busy()) {
    set_status_("rejected_busy");
    return false;
  }
  if (!valid_ || !calibrated()) {
    set_status_("rejected_invalid_feedback");
    return false;
  }
  if (target == mode_) {
    set_status_("already_satisfied");
    return true;
  }
  target_ = target;
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
  return request_({speed, mode_.light}, now);
}

bool Controller::request_light(bool light, uint32_t now) {
  tick(now);
  return request_({mode_.speed, light}, now);
}

void Controller::next_step_(uint32_t now) {
  before_ = mode_;
  expected_ = mode_;
  Command command;
  if (target_.speed != mode_.speed) {
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
  valid_ = false;
  if (validity)
    validity(false);
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
