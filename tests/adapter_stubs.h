#pragma once
// Minimal host interfaces for testing the production adapter, not the hardware.
// LightCall publication order matches ESPHome 2026.9.0 light_call.cpp.
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void) 0)
#define ESP_LOGW(...) ((void) 0)
#define ESP_LOGCONFIG(...) ((void) 0)
namespace esphome {
inline uint32_t test_millis = 0;
inline uint32_t millis() { return test_millis; }
namespace setup_priority { constexpr float LATE = -100; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  bool is_failed() const { return false; }
  bool status_has_warning() const { return false; }
};
template<typename... Ts> class Action { public: virtual ~Action() = default; virtual void play(Ts...) = 0; };
template<typename... Ts> class Trigger {
 public:
  std::function<void()> callback;
  void trigger() { if (callback) callback(); }
};
template<typename... Ts> class Automation {
 public:
  explicit Automation(Trigger<Ts...> *trigger) : trigger_(trigger) {}
  void add_action(Action<Ts...> *action) { trigger_->callback = [action]() { action->play(); }; }
 private:
  Trigger<Ts...> *trigger_;
};
namespace sensor {
class Sensor {
 public:
  float state{};
  std::function<void(float)> callback;
  void add_on_state_callback(std::function<void(float)> cb) { callback = cb; }
  void publish_state(float value) { state = value; if (callback) callback(value); }
};
}
namespace binary_sensor {
class BinarySensor { public: bool state{false}; void publish_state(bool value) { state = value; } };
}
namespace text_sensor {
class TextSensor { public: std::string state; void publish_state(const std::string &value) { state = value; } };
}
namespace button {
class Button { public: virtual ~Button() = default; void press() { press_action(); } protected: virtual void press_action() = 0; };
}
namespace fan {
class FanTraits { public: FanTraits(bool, bool, bool, int) {} };
class FanCall {
 public:
  std::optional<bool> state;
  std::optional<int> speed;
  std::optional<bool> get_state() const { return state; }
  std::optional<int> get_speed() const { return speed; }
};
class Fan {
 public:
  bool state{false}; int speed{1}; unsigned publications{0};
  virtual ~Fan() = default;
  virtual FanTraits get_traits() = 0;
  void publish_state() { ++publications; }
  void request(const FanCall &call) { control(call); }
 protected:
  virtual void control(const FanCall &call) = 0;
};
}
namespace light {
enum class ColorMode { ON_OFF };
class LightTraits { public: void set_supported_color_modes(std::initializer_list<ColorMode>) {} };
class LightValues { public: bool on{false}; bool is_on() const { return on; } void set_state(bool state) { on = state; } };
class LightState;
class LightRemoteValuesListener { public: virtual void on_light_remote_values_update() = 0; };
class LightOutput {
 public:
  virtual ~LightOutput() = default;
  virtual LightTraits get_traits() = 0;
  virtual void setup_state(LightState *) {}
  virtual void update_state(LightState *) {}
  virtual void write_state(LightState *) = 0;
};
class LightCall {
 public:
  explicit LightCall(LightState *state) : state_(state) {}
  LightCall &set_state(bool value) { value_ = value; return *this; }
  LightCall &set_transition_length(int) { return *this; }
  LightCall &set_publish(bool value) { publish_ = value; return *this; }
  LightCall &set_save(bool) { return *this; }
  void perform();
 private:
  LightState *state_; bool value_{false}, publish_{true};
};
class LightState {
 public:
  explicit LightState(LightOutput *output) : output(output) { output->setup_state(this); }
  LightValues current_values, remote_values;
  LightOutput *output;
  bool transformer{false};
  std::vector<bool> published;
  std::vector<LightRemoteValuesListener *> listeners;
  bool is_transformer_active() const { return transformer; }
  LightCall make_call() { return LightCall(this); }
  void add_remote_values_listener(LightRemoteValuesListener *listener) { listeners.push_back(listener); }
  void publish_state() {
    for (auto *listener : listeners) listener->on_light_remote_values_update();
    published.push_back(remote_values.is_on());
  }
};
inline void LightCall::perform() {
  state_->transformer = false;
  state_->current_values.set_state(value_);
  if (publish_) state_->remote_values.set_state(value_);
  state_->output->update_state(state_);
  if (publish_) state_->publish_state();
}
}
namespace remote_transmitter {
class RemoteTransmitterComponent : public Component {
 public:
  std::vector<int32_t> data;
  unsigned sends{0};
  Trigger<> completion;
  struct Data {
    RemoteTransmitterComponent *parent;
    void set_carrier_frequency(int) {}
    void set_data(const std::vector<int32_t> &value) { parent->data = value; }
  };
  struct Call {
    Data data;
    Data *get_data() { return &data; }
    void perform() { ++data.parent->sends; }
  };
  Call transmit() { return {{this}}; }
  Trigger<> *get_complete_trigger() { return &completion; }
};
}
namespace api {
struct Server {
  bool connected{true};
  bool is_connected_with_state_subscription() const { return connected; }
};
inline Server test_server;
inline Server *global_api_server = &test_server;
}
}
