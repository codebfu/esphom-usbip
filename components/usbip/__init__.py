"""USBIP component for ESPHome - exposes USB devices over TCP/IP."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import wifi, binary_sensor, text_sensor, sensor
from esphome.components.esp32 import VARIANT_ESP32S2, VARIANT_ESP32S3, only_on_variant

CONF_PORT = "port"
CONF_KEEPALIVE_IDLE = "keepalive_idle"
CONF_KEEPALIVE_INTERVAL = "keepalive_interval"
CONF_KEEPALIVE_COUNT = "keepalive_count"
CONF_ENABLE_HUBS = "enable_hubs"
CONF_SENSORS = "sensors"
CONF_DEVICE_COUNT_ID = "device_count_id"
CONF_CONNECTED_ID = "connected_id"
CONF_INUSE_ID = "inuse_id"
CONF_PID_ID = "pid_id"
CONF_BUSID_ID = "busid_id"
CONF_NAME_ID = "name_id"
CONF_IGNORE_VID_PID = "ignore_vid_pid"
CONF_IGNORE_BUSID = "ignore_busid"

SENSOR_SLOT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CONNECTED_ID): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_INUSE_ID): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_PID_ID): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_BUSID_ID): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_NAME_ID): cv.use_id(text_sensor.TextSensor),
    }
)

usbip_ns = cg.esphome_ns.namespace("usbip")
USBipComponent = usbip_ns.class_("USBipComponent", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(USBipComponent),
            cv.Optional(CONF_PORT, default=3240): cv.port,
            cv.Optional(CONF_KEEPALIVE_IDLE, default=5): cv.positive_int,
            cv.Optional(CONF_KEEPALIVE_INTERVAL, default=5): cv.positive_int,
            cv.Optional(CONF_KEEPALIVE_COUNT, default=3): cv.positive_int,
            cv.Optional(CONF_ENABLE_HUBS, default=False): cv.boolean,
            cv.Optional(CONF_DEVICE_COUNT_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_SENSORS): cv.ensure_list(SENSOR_SLOT_SCHEMA),
            cv.Optional(CONF_IGNORE_VID_PID): cv.ensure_list(cv.string),
            cv.Optional(CONF_IGNORE_BUSID): cv.ensure_list(cv.string),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.requires_component("wifi"),
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3]),
)


async def to_code(config):
    wifi.request_wifi_connect_state_listener()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_keepalive_idle(config[CONF_KEEPALIVE_IDLE]))
    cg.add(var.set_keepalive_interval(config[CONF_KEEPALIVE_INTERVAL]))
    cg.add(var.set_keepalive_count(config[CONF_KEEPALIVE_COUNT]))
    cg.add(var.set_enable_hubs(config[CONF_ENABLE_HUBS]))

    if CONF_DEVICE_COUNT_ID in config:
        sens = await cg.get_variable(config[CONF_DEVICE_COUNT_ID])
        cg.add(var.set_device_count_sensor(sens))

    for vid_pid in config.get(CONF_IGNORE_VID_PID, []):
        cg.add(var.add_ignore_vid_pid(vid_pid))
    for busid in config.get(CONF_IGNORE_BUSID, []):
        cg.add(var.add_ignore_busid(busid))

    if CONF_SENSORS in config:
        for slot_config in config[CONF_SENSORS]:
            connected = await cg.get_variable(slot_config[CONF_CONNECTED_ID]) if CONF_CONNECTED_ID in slot_config else None
            inuse = await cg.get_variable(slot_config[CONF_INUSE_ID]) if CONF_INUSE_ID in slot_config else None
            pid = await cg.get_variable(slot_config[CONF_PID_ID]) if CONF_PID_ID in slot_config else None
            busid = await cg.get_variable(slot_config[CONF_BUSID_ID]) if CONF_BUSID_ID in slot_config else None
            name = await cg.get_variable(slot_config[CONF_NAME_ID]) if CONF_NAME_ID in slot_config else None
            cg.add(var.add_sensor_slot(connected, inuse, pid, busid, name))
