"""Novy RF controller with measured power feedback (ESPHome 2026.9.0)."""

import math

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, button, fan, light, remote_transmitter, sensor, text_sensor
from esphome.const import CONF_ID, CONF_OUTPUT_ID

DEPENDENCIES = ["esp32", "api", "remote_transmitter"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "fan", "light", "button"]
MULTI_CONF = False

ns = cg.esphome_ns.namespace("novy")
NovyComponent = ns.class_("NovyComponent", cg.Component)
NovyFan = ns.class_("NovyFan", fan.Fan)
NovyLight = ns.class_("NovyLight", light.LightOutput)
NovyButton = ns.class_("NovyButton", button.Button)
Command = ns.enum("Command", is_class=True)
COMMANDS = {name: getattr(Command, name.upper()) for name in ("light", "power", "plus", "minus", "novy")}


def finite_power(value):
    value = cv.float_(value)
    if not math.isfinite(value):
        raise cv.Invalid("Power must be finite")
    if value < 0:
        raise cv.Invalid("Power must be non-negative")
    return value


ENTRY = cv.Schema({
    cv.Required("speed"): cv.int_range(min=0, max=4),
    cv.Required("light"): cv.boolean,
    cv.Required("min_power"): finite_power,
    cv.Required("max_power"): finite_power,
})


def validate_calibration(entries):
    if not entries:
        return entries
    if len(entries) != 10 or len({(e["speed"], e["light"]) for e in entries}) != 10:
        raise cv.Invalid("Provide all ten unique speed/light combinations, or [] for commissioning")
    for entry in entries:
        if entry["max_power"] < entry["min_power"]:
            raise cv.Invalid("max_power must be >= min_power")
    # Overlap is allowed: the runtime must report ambiguity instead of guessing.
    return entries


def validate_settings(config):
    window = config["averaging_window"].total_milliseconds
    settle = config["settle_time"].total_milliseconds
    timeout = config["step_timeout"].total_milliseconds
    if config["stale_timeout"].total_milliseconds <= window:
        raise cv.Invalid("stale_timeout must exceed averaging_window")
    if timeout <= settle + 2 * window:
        raise cv.Invalid("step_timeout must exceed settle_time plus two averaging windows")
    if config["light"].get("effects"):
        raise cv.Invalid("Novy light supports on/off only, without effects")
    if config["light"]["restore_mode"] != "ALWAYS_OFF":
        raise cv.Invalid("Novy light restore_mode must be ALWAYS_OFF; observation establishes real state")
    if config["fan"]["restore_mode"] != "NO_RESTORE":
        raise cv.Invalid("Novy fan restore_mode must be NO_RESTORE; observation establishes real state")
    return config


def duration(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 1 <= value.total_milliseconds <= 3600000:
        raise cv.Invalid("Timing must be between 1ms and 1h")
    return value


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(NovyComponent),
    cv.Required("transmitter_id"): cv.use_id(remote_transmitter.RemoteTransmitterComponent),
    cv.Required("power_sensor_id"): cv.use_id(sensor.Sensor),
    cv.Optional("pairing_code", default=1): cv.int_range(min=1, max=10),
    cv.Optional("calibration", default=[]): cv.All(cv.ensure_list(ENTRY), validate_calibration),
    cv.Optional("averaging_window", default="3s"): duration,
    cv.Optional("stale_timeout", default="30s"): duration,
    cv.Optional("settle_time", default="3s"): duration,
    cv.Optional("step_timeout", default="30s"): duration,
    cv.Required("fan"): fan.fan_schema(NovyFan, default_restore_mode="NO_RESTORE"),
    cv.Required("light"): light.light_schema(NovyLight, light.LightType.BINARY, default_restore_mode="ALWAYS_OFF"),
    cv.Required("raw_buttons"): cv.Schema({
        cv.Required(name): button.button_schema(NovyButton).extend({cv.Optional("disabled_by_default", default=True): cv.boolean})
        for name in COMMANDS
    }),
    cv.Required("average_power"): sensor.sensor_schema(unit_of_measurement="W", accuracy_decimals=1,
                                                     device_class="power", state_class="measurement"),
    cv.Required("reading_age"): sensor.sensor_schema(unit_of_measurement="s", accuracy_decimals=0,
                                                   entity_category="diagnostic"),
    cv.Required("feedback_valid"): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
    cv.Required("inferred_mode"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
    cv.Required("command_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
}).extend(cv.COMPONENT_SCHEMA), validate_settings)


def validate_transmitter(config):
    full = fv.full_config.get()
    path = full.get_path_for_id(config["transmitter_id"])[:-1]
    transmitter = full.get_config_for_path(path)
    if transmitter.get("on_complete") or transmitter.get("on_transmit"):
        raise cv.Invalid("Use a dedicated Novy transmitter without on_complete/on_transmit automations")
    if transmitter["carrier_duty_percent"] != 100 or transmitter["pin"].get("inverted", False):
        raise cv.Invalid("Novy requires carrier_duty_percent: 100% and a non-inverted DATA pin")
    if transmitter.get("eot_level", False) or not transmitter.get("non_blocking", False):
        raise cv.Invalid("Novy requires non_blocking: true and eot_level: false on an ESP32 with RMT")
    return config


FINAL_VALIDATE_SCHEMA = validate_transmitter


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_transmitter(await cg.get_variable(config["transmitter_id"])))
    cg.add(var.set_power_sensor(await cg.get_variable(config["power_sensor_id"])))
    cg.add(var.set_pairing_code(config["pairing_code"]))
    cg.add(var.set_timing(*(config[key].total_milliseconds for key in
                           ("averaging_window", "stale_timeout", "settle_time", "step_timeout"))))
    for entry in config["calibration"]:
        cg.add(var.add_calibration(entry["speed"], entry["light"], entry["min_power"], entry["max_power"]))
    fan_var = await fan.new_fan(config["fan"], var)
    cg.add(var.set_fan(fan_var))
    light_var = cg.new_Pvariable(config["light"][CONF_OUTPUT_ID], var)
    await light.register_light(light_var, config["light"])
    cg.add(var.set_light(light_var))
    for name, command in COMMANDS.items():
        await button.new_button(config["raw_buttons"][name], var, command)
    for key, setter in (("average_power", "set_average_sensor"), ("reading_age", "set_age_sensor")):
        cg.add(getattr(var, setter)(await sensor.new_sensor(config[key])))
    cg.add(var.set_valid_sensor(await binary_sensor.new_binary_sensor(config["feedback_valid"])))
    for key, setter in (("inferred_mode", "set_mode_sensor"), ("command_status", "set_status_sensor")):
        cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))
