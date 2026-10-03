#pragma once

#include "controller.h"
#include "jsy.h"
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/remote_transmitter/remote_transmitter.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome::novy {

class NovyComponent;

class NovyFan : public fan::Fan {
 public:
  explicit NovyFan(NovyComponent *parent) : parent_(parent) {}
  fan::FanTraits get_traits() override { return fan::FanTraits(false, true, false, 4); }
  void observe(Mode mode);
 protected:
  void control(const fan::FanCall &call) override;
  NovyComponent *parent_;
};

class NovyLight : public light::LightOutput, public light::LightRemoteValuesListener {
 public:
  explicit NovyLight(NovyComponent *parent) : parent_(parent) {}
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override {
    state_ = state;
    state_->add_remote_values_listener(this);
  }
  void on_light_remote_values_update() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *) override {}  // RF only originates from requests.
  void observe(bool value);
 protected:
  void restore_observed_();
  NovyComponent *parent_;
  light::LightState *state_{nullptr};
  bool observed_{false};
  bool restoring_{false};
};

class NovyButton : public button::Button {
 public:
  NovyButton(NovyComponent *parent, Command command) : parent_(parent), command_(command) {}
 protected:
  void press_action() override;
  NovyComponent *parent_;
  Command command_;
};

class NovyComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void set_transmitter(remote_transmitter::RemoteTransmitterComponent *value) { transmitter_ = value; }
  void set_pairing_code(unsigned value) { pairing_code_ = value; }
  // JSY-MK-194G channels are numbered 1 and 2.
  void set_channels(unsigned fan, unsigned light) {
    fan_channel_ = fan - 1;
    light_channel_ = light - 1;
  }
  void set_update_interval(uint32_t value) { update_interval_ = value; }
  void set_power_waveform(const std::vector<int32_t> &value) { power_waveform_ = value; }
  void set_fan(NovyFan *value) { fan_ = value; }
  void set_light(NovyLight *value) { light_ = value; }
  void set_fan_power_sensor(sensor::Sensor *value) { fan_power_ = value; }
  void set_light_power_sensor(sensor::Sensor *value) { light_power_ = value; }
  void set_age_sensor(sensor::Sensor *value) { age_ = value; }
  void set_valid_sensor(binary_sensor::BinarySensor *value) { valid_ = value; }
  void set_speed_valid_sensor(binary_sensor::BinarySensor *value) { speed_valid_ = value; }
  void set_light_valid_sensor(binary_sensor::BinarySensor *value) { light_valid_ = value; }
  void set_mode_sensor(text_sensor::TextSensor *value) { mode_ = value; }
  void set_status_sensor(text_sensor::TextSensor *value) { status_ = value; }
  void set_timing(uint32_t window, uint32_t stale, uint32_t settle, uint32_t timeout) {
    controller_.timing = {window, stale, settle, timeout};
  }
  void add_fan_range(int speed, float low, float high) { controller_.fan_calibration.push_back({speed, low, high}); }
  void set_light_threshold(float value) { controller_.light_threshold = value; }
  void request_speed(int speed);
  void request_light(bool light);
  void raw(Command command);
  void transmission_done();
  bool ready() const { return ready_; }

 protected:
  void send_(Command command);
  void read_meter_(uint32_t now);
  Controller controller_;
  remote_transmitter::RemoteTransmitterComponent *transmitter_{nullptr};
  sensor::Sensor *fan_power_{nullptr}, *light_power_{nullptr}, *age_{nullptr};
  binary_sensor::BinarySensor *valid_{nullptr}, *speed_valid_{nullptr}, *light_valid_{nullptr};
  text_sensor::TextSensor *mode_{nullptr}, *status_{nullptr};
  NovyFan *fan_{nullptr};
  NovyLight *light_{nullptr};
  unsigned pairing_code_{1};
  std::vector<int32_t> power_waveform_;
  unsigned fan_channel_{1}, light_channel_{0};
  uint32_t update_interval_{500}, last_poll_{0};
  uint8_t response_[JSY194_RESPONSE_SIZE]{};
  size_t received_{0};
  bool ready_{false}, polling_{false};
  uint32_t last_age_publish_{0};
};

class CompleteAction : public Action<> {
 public:
  explicit CompleteAction(NovyComponent *parent) : parent_(parent) {}
  void play() override { parent_->transmission_done(); }
 protected:
  NovyComponent *parent_;
};

}  // namespace esphome::novy
