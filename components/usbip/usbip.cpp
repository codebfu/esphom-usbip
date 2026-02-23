#ifdef USE_ESP32

#include <cstdio>
#include <span>
#include <string>

#include "esphome/core/application.h"
#include "usbip.h"
#include "tcp_server.hpp"
#include "usbip_protocol.hpp"

namespace esphome {
namespace usbip {

static const char *const TAG = "usbip";

static USBipApplication s_usbip_app;
static USBipComponent *s_entity_component = nullptr;

static void on_connected_cb(const char *busid, uint16_t vid, uint16_t pid, const char *name) {
  if (s_entity_component) {
    s_entity_component->schedule_on_connected(busid, vid, pid, name);
  }
}

static void on_disconnected_cb(const char *busid) {
  if (s_entity_component) {
    s_entity_component->schedule_on_disconnected(busid);
  }
}

static void on_attached_cb(const char *busid) {
  if (s_entity_component) {
    s_entity_component->schedule_on_attached(busid);
  }
}

static void on_released_cb(const char *busid) {
  if (s_entity_component) {
    s_entity_component->schedule_on_released(busid);
  }
}

void USBipComponent::setup() {
  s_entity_component = this;
  s_usbip_app.set_callbacks(on_connected_cb, on_disconnected_cb, on_attached_cb, on_released_cb);
  s_usbip_app.init(enable_hubs_);

  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->add_connect_state_listener(this);
    if (wifi::global_wifi_component->is_connected()) {
      const std::string &ssid = wifi::global_wifi_component->get_sta().get_ssid();
      const wifi::bssid_t &bssid = wifi::global_wifi_component->get_sta().get_bssid();
      if (!ssid.empty()) {
        this->on_wifi_connect_state(StringRef(ssid.c_str(), ssid.length()),
                                   std::span<const uint8_t, 6>(bssid.data(), 6));
      } else {
        static constexpr uint8_t EMPTY_BSSID[6] = {};
        this->on_wifi_connect_state(StringRef("connected", 9), EMPTY_BSSID);
      }
    }
  }
}

void USBipComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USBIP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %d", port_);
  ESP_LOGCONFIG(TAG, "  Keepalive: idle=%ds interval=%ds count=%d", keepalive_idle_,
                keepalive_interval_, keepalive_count_);
}

void USBipComponent::schedule_on_connected(const char *busid, uint16_t vid, uint16_t pid,
                                            const char *name) {
  std::string b(busid);
  std::string n(name ? name : "");
  set_timeout("usbip_conn", 0, [this, b, n, vid, pid]() {
    on_device_connected(b.c_str(), vid, pid, n.c_str());
  });
}

void USBipComponent::schedule_on_disconnected(const char *busid) {
  std::string b(busid);
  set_timeout("usbip_disc", 0, [this, b]() { on_device_disconnected(b.c_str()); });
}

void USBipComponent::schedule_on_attached(const char *busid) {
  std::string b(busid);
  set_timeout("usbip_att", 0, [this, b]() { on_device_attached(b.c_str()); });
}

void USBipComponent::schedule_on_released(const char *busid) {
  std::string b(busid);
  set_timeout("usbip_rel", 0, [this, b]() { on_device_released(b.c_str()); });
}

void USBipComponent::on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) {
  if (server_started_)
    return;
  if (ssid.empty())
    return;

  server_started_ = true;

  TcpServerConfig config = {
      .port = port_,
      .keepalive_idle = keepalive_idle_,
      .keepalive_interval = keepalive_interval_,
      .keepalive_count = keepalive_count_,
      .socket_close_callback = usbip_on_socket_close,
  };
  tcp_server_start(&config);

  ESP_LOGI(TAG, "USBIP TCP server started on port %d", port_);
}

void USBipComponent::update_device(const char *busid, uint16_t vid, uint16_t pid, const char *name,
                                   bool connected, bool inuse) {
  if (is_device_ignored(busid, vid, pid))
    return;
  auto it = busid_to_index_.find(busid);
  if (it != busid_to_index_.end()) {
    size_t idx = it->second;
    if (idx < devices_.size()) {
      devices_[idx].vid = vid;
      devices_[idx].pid = pid;
      devices_[idx].name = name ? name : "";
      devices_[idx].connected = connected;
    }
    return;
  }
  if (!connected)
    return;
  DeviceInfo info;
  info.busid = busid;
  info.name = name ? name : "";
  info.vid = vid;
  info.pid = pid;
  info.connected = true;
  info.inuse = false;
  size_t idx = devices_.size();
  devices_.push_back(info);
  busid_to_index_[busid] = idx;
}

void USBipComponent::add_ignore_vid_pid(const std::string &vid_pid) {
  ignore_vid_pid_.push_back(vid_pid);
}

void USBipComponent::add_ignore_busid(const std::string &busid) {
  ignore_busid_.push_back(busid);
}

bool USBipComponent::is_device_ignored(const char *busid, uint16_t vid, uint16_t pid) const {
  for (const auto &b : ignore_busid_) {
    if (b == busid)
      return true;
  }
  char vid_pid_buf[32];
  snprintf(vid_pid_buf, sizeof(vid_pid_buf), "0x%04x:0x%04x", vid, pid);
  for (const auto &pattern : ignore_vid_pid_) {
    if (pattern == vid_pid_buf)
      return true;
    size_t colon = pattern.find(':');
    if (colon != std::string::npos && colon + 2 <= pattern.size()) {
      std::string suffix = pattern.substr(colon + 1);
      if (suffix == "*") {
        unsigned int pvid = 0;
        if (sscanf(pattern.c_str(), "0x%04x:", &pvid) == 1 && (uint16_t) pvid == vid)
          return true;
      }
    }
  }
  return false;
}

void USBipComponent::add_sensor_slot(binary_sensor::BinarySensor *connected,
                                     binary_sensor::BinarySensor *inuse,
                                     text_sensor::TextSensor *vid_pid,
                                     text_sensor::TextSensor *busid,
                                     text_sensor::TextSensor *name) {
  sensor_slots_.push_back({connected, inuse, vid_pid, busid, name});
}

void USBipComponent::update_sensors_for_index(size_t index) {
  if (index >= sensor_slots_.size())
    return;
  const SensorSlot &slot = sensor_slots_[index];
  bool connected = (index < devices_.size()) && devices_[index].connected;
  bool inuse = (index < devices_.size()) && devices_[index].inuse;
  std::string vid_pid_str = get_device_vid_pid(index);
  std::string busid_str = get_device_busid(index);
  std::string name_str = get_device_name(index);

  if (slot.connected)
    slot.connected->publish_state(connected);
  if (slot.inuse)
    slot.inuse->publish_state(inuse);
  if (slot.vid_pid)
    slot.vid_pid->publish_state(vid_pid_str);
  if (slot.busid)
    slot.busid->publish_state(busid_str);
  if (slot.name)
    slot.name->publish_state(name_str);
}

void USBipComponent::update_all_sensors() {
  size_t max_idx =
      sensor_slots_.size() > devices_.size() ? sensor_slots_.size() : devices_.size();
  for (size_t i = 0; i < max_idx; i++) {
    update_sensors_for_index(i);
  }
  if (device_count_sensor_)
    device_count_sensor_->publish_state(devices_.size());
}

void USBipComponent::on_device_connected(const char *busid, uint16_t vid, uint16_t pid,
                                         const char *name) {
  ESP_LOGI(TAG, "USB device connected: %s (vid=0x%04x pid=0x%04x) %s", busid, vid, pid,
           name && name[0] ? name : "");
  update_device(busid, vid, pid, name, true, false);
  set_timeout("usbip_sensors", 0, [this]() { update_all_sensors(); });
}

void USBipComponent::on_device_disconnected(const char *busid) {
  auto it = busid_to_index_.find(busid);
  if (it != busid_to_index_.end()) {
    size_t idx = it->second;
    busid_to_index_.erase(it);
    if (idx < devices_.size()) {
      devices_.erase(devices_.begin() + idx);
      for (auto &kv : busid_to_index_) {
        if (kv.second > idx)
          kv.second--;
      }
    }
    set_timeout("usbip_sensors", 0, [this]() { update_all_sensors(); });
  }
}

void USBipComponent::on_device_attached(const char *busid) {
  auto it = busid_to_index_.find(busid);
  if (it != busid_to_index_.end() && it->second < devices_.size()) {
    devices_[it->second].inuse = true;
    set_timeout("usbip_sensors", 0, [this]() { update_all_sensors(); });
  }
}

void USBipComponent::on_device_released(const char *busid) {
  auto it = busid_to_index_.find(busid);
  if (it != busid_to_index_.end() && it->second < devices_.size()) {
    devices_[it->second].inuse = false;
    set_timeout("usbip_sensors", 0, [this]() { update_all_sensors(); });
  }
}

static void refresh_device_cb(const char *busid, uint16_t vid, uint16_t pid, const char *name,
                             void *arg) {
  USBipComponent *comp = (USBipComponent *)arg;
  if (comp && comp->is_device_ignored(busid, vid, pid))
    return;
  ESP_LOGI(TAG, "  Device: %s vid=0x%04x pid=0x%04x %s", busid, vid, pid,
           name && name[0] ? name : "");
  if (comp) {
    comp->schedule_on_connected(busid, vid, pid, name);
  }
}

void USBipComponent::refresh_devices() {
  ESP_LOGI(TAG, "=== Refresh USB devices ===");
  int num_on_bus = 0;
  s_usbip_app.refresh_device_list(refresh_device_cb, this, &num_on_bus);
  ESP_LOGI(TAG, "  Devices on USB bus: %d", num_on_bus);
  set_timeout("usbip_sensors", 0, [this]() {
    ESP_LOGI(TAG, "Known devices (%zu):", devices_.size());
    for (size_t i = 0; i < devices_.size(); i++) {
      const auto &d = devices_[i];
      ESP_LOGI(TAG, "  [%zu] %s 0x%04x:0x%04x %s (connected=%d inuse=%d)", i, d.busid.c_str(),
               d.vid, d.pid, d.name.empty() ? "" : d.name.c_str(), d.connected, d.inuse);
    }
    update_all_sensors();
  });
  ESP_LOGI(TAG, "=== End refresh ===");
}

std::string USBipComponent::get_device_vid_pid(int index) const {
  if (index < 0 || (size_t)index >= devices_.size())
    return "";
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%04x:0x%04x", devices_[index].vid, devices_[index].pid);
  return std::string(buf);
}

std::string USBipComponent::get_device_busid(int index) const {
  if (index < 0 || (size_t)index >= devices_.size())
    return "";
  return devices_[index].busid;
}

std::string USBipComponent::get_device_name(int index) const {
  if (index < 0 || (size_t)index >= devices_.size())
    return "";
  return devices_[index].name;
}

bool USBipComponent::get_device_connected(int index) const {
  if (index < 0 || (size_t)index >= devices_.size())
    return false;
  return devices_[index].connected;
}

bool USBipComponent::get_device_inuse(int index) const {
  if (index < 0 || (size_t)index >= devices_.size())
    return false;
  return devices_[index].inuse;
}

std::string USBipComponent::get_busid_by_vid_pid(uint16_t vid, uint16_t pid) const {
  for (const auto &d : devices_) {
    if (d.vid == vid && d.pid == pid)
      return d.busid;
  }
  return "";
}

std::string USBipComponent::get_name_by_vid_pid(uint16_t vid, uint16_t pid) const {
  for (const auto &d : devices_) {
    if (d.vid == vid && d.pid == pid)
      return d.name;
  }
  return "";
}

bool USBipComponent::get_connected_by_vid_pid(uint16_t vid, uint16_t pid) const {
  for (const auto &d : devices_) {
    if (d.vid == vid && d.pid == pid)
      return d.connected;
  }
  return false;
}

bool USBipComponent::get_inuse_by_vid_pid(uint16_t vid, uint16_t pid) const {
  for (const auto &d : devices_) {
    if (d.vid == vid && d.pid == pid)
      return d.inuse;
  }
  return false;
}

std::string USBipComponent::get_device_vid_pid_by_busid(const std::string &busid) const {
  auto it = busid_to_index_.find(busid);
  if (it == busid_to_index_.end() || it->second >= devices_.size())
    return "";
  return get_device_vid_pid(it->second);
}

std::string USBipComponent::get_device_name_by_busid(const std::string &busid) const {
  auto it = busid_to_index_.find(busid);
  if (it == busid_to_index_.end() || it->second >= devices_.size())
    return "";
  return devices_[it->second].name;
}

bool USBipComponent::get_device_connected_by_busid(const std::string &busid) const {
  auto it = busid_to_index_.find(busid);
  if (it == busid_to_index_.end() || it->second >= devices_.size())
    return false;
  return devices_[it->second].connected;
}

bool USBipComponent::get_device_inuse_by_busid(const std::string &busid) const {
  auto it = busid_to_index_.find(busid);
  if (it == busid_to_index_.end() || it->second >= devices_.size())
    return false;
  return devices_[it->second].inuse;
}

}  // namespace usbip
}  // namespace esphome

#endif
