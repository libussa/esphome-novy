#include "novy.h"
#include "esphome/components/api/api_server.h"
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
    mode_->publish_state(valid ? mode_name(controller_.mode()) : "unknown");
  };
  controller_.status = [this](const std::string &value) {
    ESP_LOGI(TAG, "%s", value.c_str());
    status_->publish_state(value);
  };
  controller_.averaged = [this](float watts) { average_->publish_state(watts); };
  power_->add_on_state_callback([this](float watts) {
    controller_.sample(watts, millis());
  });
  controller_.start(millis());
  age_->publish_state(NAN);
  ready_ = true;
}

void NovyComponent::loop() {
  auto now = millis();
  // A log-only API client does not count as an HA state connection. Source
  // silence is independently detected even when another state client is present.
  bool connected = api::global_api_server->is_connected_with_state_subscription();
  if (connected_ && !connected)
    controller_.disconnect(now);
  connected_ = connected;
  controller_.tick(now);
  if (now - last_age_publish_ >= 1000) {
    last_age_publish_ = now;
    age_->publish_state(controller_.age(now));
  }
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
  call.get_data()->set_data(waveform(command, pairing_code_));
  call.perform();
}

void NovyComponent::transmission_done() {
  // RMT completion proves transmission finished, not that the hood received it.
  controller_.transmission_done(millis(), !transmitter_->is_failed() && !transmitter_->status_has_warning());
}

void NovyComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Novy controller: pairing code %u, calibration %s", pairing_code_,
                controller_.calibrated() ? "complete" : "missing");
}
}  // namespace esphome::novy
