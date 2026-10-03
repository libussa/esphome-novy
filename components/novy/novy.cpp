#include "novy.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::novy {
static const char *const TAG = "novy";

void NovyFan::control(const fan::FanCall &call) {
  if (call.get_state().has_value() && !*call.get_state()) {
    parent_->request_speed(0);
  } else {
    int requested = call.get_speed().value_or(this->state ? this->speed : 1);
    parent_->request_speed(requested);
  }
  // Also correct any HA optimistic display after a rejected request.
  this->publish_state();
}

void NovyFan::observe(Mode mode) {
  this->state = mode.speed > 0;
  this->speed = mode.speed > 0 ? mode.speed : 1;
  this->publish_state();
}

light::LightTraits NovyLight::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void NovyLight::restore_observed_() {
  // LightCall publishes remote_values after update_state returns. Restore both
  // here, before that publication, so an unconfirmed target is never the state.
  state_->current_values.set_state(observed_);
  state_->remote_values.set_state(observed_);
}

void NovyLight::update_state(light::LightState *state) {
  bool requested = state->current_values.is_on();
  restore_observed_();
  // Startup restoration must not turn into a hood command. Flash/transition
  // effects are unsupported; the adapter never sends their intermediate states.
  if (parent_->ready() && !restoring_ && !state->is_transformer_active())
    parent_->request_light(requested);
}

void NovyLight::on_light_remote_values_update() {
  if (state_->is_transformer_active()) {
    // Flash bypasses update_state initially. Cancel it before the API registry
    // is notified and before its transformer can produce future RF requests.
    restoring_ = true;
    state_->make_call().set_state(observed_).set_transition_length(0).set_publish(false).set_save(false).perform();
    restoring_ = false;
    restore_observed_();
    ESP_LOGW(TAG, "Light flashes/transitions are unsupported");
  }
}

void NovyLight::observe(bool value) {
  observed_ = value;
  if (state_ != nullptr) {
    restore_observed_();
    state_->publish_state();
  }
}

void NovyButton::press_action() { parent_->raw(command_); }

void NovyComponent::setup() {
  // This transmitter is dedicated to Novy; schema validation rejects user
  // on_complete automations that would compete for its single automation slot.
  auto *completion = new Automation<>(transmitter_->get_complete_trigger());
  completion->add_action(new CompleteAction(this));
  controller_.transmit = [this](Command command) { this->send_(command); };
  controller_.observed = [this](Mode mode) {
    fan_->observe(mode);
    light_->observe(mode.light);
  };
  controller_.validity = [this](bool valid) {
    valid_->publish_state(valid);
    mode_->publish_state(controller_.inferred_mode());
  };
  controller_.confidence = [this](bool speed, bool light) {
    if (speed_valid_ != nullptr)
      speed_valid_->publish_state(speed);
    if (light_valid_ != nullptr)
      light_valid_->publish_state(light);
  };
  controller_.status = [this](const std::string &value) {
    ESP_LOGI(TAG, "%s", value.c_str());
    status_->publish_state(value);
  };
  controller_.averaged = [this](float fan, float light) {
    fan_power_->publish_state(fan);
    light_power_->publish_state(light);
  };
  controller_.start(millis());
  age_->publish_state(NAN);
  ready_ = true;
}

void NovyComponent::loop() {
  auto now = millis();
  read_meter_(now);
  controller_.tick(now);
  if (now - last_age_publish_ >= 1000) {
    last_age_publish_ = now;
    age_->publish_state(controller_.age(now));
  }
}

void NovyComponent::read_meter_(uint32_t now) {
  while (polling_ && received_ < JSY194_RESPONSE_SIZE && this->available() &&
         this->read_byte(&response_[received_]))
    ++received_;
  if (polling_ && received_ == JSY194_RESPONSE_SIZE) {
    polling_ = false;
    float power[2];
    // A failed read supplies no sample; persistent failure empties the
    // averaging windows, which invalidates feedback.
    if (parse_jsy194(response_, power))
      controller_.sample(power[fan_channel_], power[light_channel_], now);
    else
      ESP_LOGW(TAG, "Invalid JSY-MK-194G response");
  }
  if (now - last_poll_ < update_interval_)
    return;
  if (polling_)
    ESP_LOGW(TAG, "JSY-MK-194G response timeout after %u bytes", static_cast<unsigned>(received_));
  // Drop late or unsolicited bytes so the next response starts aligned.
  uint8_t discarded;
  while (this->available() && this->read_byte(&discarded))
    continue;
  this->write_array(jsy194_request());
  last_poll_ = now;
  polling_ = true;
  received_ = 0;
}

void NovyComponent::request_speed(int speed) {
  if (ready_)
    controller_.request_speed(speed, millis());
}
void NovyComponent::request_light(bool light) {
  if (ready_)
    controller_.request_light(light, millis());
}
void NovyComponent::raw(Command command) {
  if (ready_)
    controller_.raw(command, millis());
}

void NovyComponent::send_(Command command) {
  if (transmitter_->is_failed()) {
    controller_.transmission_done(millis(), false);
    return;
  }
  auto call = transmitter_->transmit();
  call.get_data()->set_carrier_frequency(0);
  call.get_data()->set_data(command == Command::POWER && !power_waveform_.empty()
                               ? power_waveform_ : waveform(command, pairing_code_));
  call.perform();
}

void NovyComponent::transmission_done() {
  // RMT completion proves transmission finished, not that the hood received it.
  controller_.transmission_done(millis(), !transmitter_->is_failed() && !transmitter_->status_has_warning());
}

void NovyComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Novy controller: pairing code %u, calibration %s", pairing_code_,
                controller_.calibrated() ? "complete" : "missing");
  ESP_LOGCONFIG(TAG, "  Power waveform: %s", power_waveform_.empty() ? "built-in" : "captured override");
  ESP_LOGCONFIG(TAG, "  JSY-MK-194G: fan channel %u, light channel %u, every %ums", fan_channel_ + 1,
                light_channel_ + 1, static_cast<unsigned>(update_interval_));
}
}  // namespace esphome::novy
