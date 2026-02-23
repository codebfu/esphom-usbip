#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/wifi/wifi_component.h"

#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace usbip {

struct SensorSlot {
  binary_sensor::BinarySensor *connected{nullptr};
  binary_sensor::BinarySensor *inuse{nullptr};
  text_sensor::TextSensor *vid_pid{nullptr};
  text_sensor::TextSensor *busid{nullptr};
  text_sensor::TextSensor *name{nullptr};
};

class USBipComponent : public Component, public wifi::WiFiConnectStateListener {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { port_ = port; }
  void set_keepalive_idle(int v) { keepalive_idle_ = v; }
  void set_keepalive_interval(int v) { keepalive_interval_ = v; }
  void set_keepalive_count(int v) { keepalive_count_ = v; }
  void set_enable_hubs(bool v) { enable_hubs_ = v; }

  void set_device_count_sensor(sensor::Sensor *s) { device_count_sensor_ = s; }
  void add_ignore_vid_pid(const std::string &vid_pid);
  void add_ignore_busid(const std::string &busid);
  void add_sensor_slot(binary_sensor::BinarySensor *connected,
                       binary_sensor::BinarySensor *inuse,
                       text_sensor::TextSensor *vid_pid,
                       text_sensor::TextSensor *busid,
                       text_sensor::TextSensor *name);

  void on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) override;

  void schedule_on_connected(const char *busid, uint16_t vid, uint16_t pid, const char *name);
  void schedule_on_disconnected(const char *busid);
  void schedule_on_attached(const char *busid);
  void schedule_on_released(const char *busid);

  void refresh_devices();

  // Getters by device index (0-based)
  std::string get_device_vid_pid(int index) const;
  std::string get_device_busid(int index) const;
  std::string get_device_name(int index) const;
  bool get_device_connected(int index) const;
  bool get_device_inuse(int index) const;

  // Getters by vid:pid (returns first match if multiple)
  std::string get_busid_by_vid_pid(uint16_t vid, uint16_t pid) const;
  std::string get_name_by_vid_pid(uint16_t vid, uint16_t pid) const;
  bool get_connected_by_vid_pid(uint16_t vid, uint16_t pid) const;
  bool get_inuse_by_vid_pid(uint16_t vid, uint16_t pid) const;

  // Getters by busid
  std::string get_device_vid_pid_by_busid(const std::string &busid) const;
  std::string get_device_name_by_busid(const std::string &busid) const;
  bool get_device_connected_by_busid(const std::string &busid) const;
  bool get_device_inuse_by_busid(const std::string &busid) const;

  int get_device_count() const { return devices_.size(); }

  bool is_device_ignored(const char *busid, uint16_t vid, uint16_t pid) const;

 protected:
  void on_device_connected(const char *busid, uint16_t vid, uint16_t pid, const char *name);
  void on_device_disconnected(const char *busid);
  void on_device_attached(const char *busid);
  void on_device_released(const char *busid);

  void update_device(const char *busid, uint16_t vid, uint16_t pid, const char *name,
                     bool connected, bool inuse);
  void update_sensors_for_index(size_t index);
  void update_all_sensors();

  uint16_t port_{3240};
  int keepalive_idle_{5};
  int keepalive_interval_{5};
  int keepalive_count_{3};
  bool enable_hubs_{false};
  bool server_started_{false};

  struct DeviceInfo {
    std::string busid;
    std::string name;
    uint16_t vid{0};
    uint16_t pid{0};
    bool connected{false};
    bool inuse{false};
  };
  std::vector<DeviceInfo> devices_;
  std::map<std::string, size_t> busid_to_index_;
  std::vector<std::string> ignore_vid_pid_;
  std::vector<std::string> ignore_busid_;

  sensor::Sensor *device_count_sensor_{nullptr};
  std::vector<SensorSlot> sensor_slots_;
};

}  // namespace usbip
}  // namespace esphome

#endif
