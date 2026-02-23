#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#include "usb_host.h"

void _client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg) {
  USBhost *host = (USBhost *)arg;
  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    host->open(event_msg);
    ESP_LOGI("usb_host", "client event: %d, address: %d", event_msg->event,
             event_msg->new_dev.address);
    if (host->_client_event_cb) {
      host->_client_event_cb(event_msg, arg);
    }
  } else {
    ESP_LOGI("usb_host", "client event: %d", event_msg->event);
    if (host->_client_event_cb) {
      host->_client_event_cb(event_msg, arg);
    }
    host->close();
  }
}

static void client_async_seq_task(void *param) {
  USBhost *host = (USBhost *)param;
  while (1) {
    usb_host_client_handle_t client_hdl = host->client_hdl;
    uint32_t event_flags;
    if (client_hdl)
      usb_host_client_handle_events(client_hdl, 1);
    if (ESP_OK == usb_host_lib_handle_events(1, &event_flags)) {
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        do {
          if (usb_host_device_free_all() != ESP_ERR_NOT_FINISHED)
            break;
        } while (1);
        usb_host_uninstall();
        host->init(false);
      }
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
        usb_host_client_deregister(client_hdl);
        host->client_hdl = NULL;
      }
    }
  }
  vTaskDelete(NULL);
}

USBhost::USBhost() {}

USBhost::~USBhost() {}

bool USBhost::init(bool create_tasks, bool enable_hubs) {
  const usb_host_config_t config = {
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  (void)enable_hubs;
  esp_err_t err = usb_host_install(&config);

  const usb_host_client_config_t client_config = {
      .max_num_event_msg = 15,
      .async =
          {
              .client_event_callback = _client_event_callback,
              .callback_arg = this,
          },
  };

  err = usb_host_client_register(&client_config, &client_hdl);

  if (create_tasks) {
    xTaskCreate(client_async_seq_task, "usb_host_async", 6 * 512, this, 20, NULL);
  }

  return true;
}

bool USBhost::open(const usb_host_client_event_msg_t *event_msg) {
  esp_err_t err = usb_host_device_open(client_hdl, event_msg->new_dev.address, &dev_hdl);
  return err == ESP_OK;
}

void USBhost::close() { usb_host_device_close(client_hdl, dev_hdl); }

usb_device_info_t USBhost::getDeviceInfo() {
  usb_host_device_info(dev_hdl, &dev_info);
  return dev_info;
}

static std::string usb_str_desc_to_string(const usb_str_desc_t *str_desc) {
  if (str_desc == nullptr || str_desc->bLength < 2)
    return "";
  std::string result;
  int n = (str_desc->bLength - 2) / 2;
  for (int i = 0; i < n; i++) {
    uint16_t w = str_desc->wData[i];
    if (w <= 0xFF)
      result += (char)w;
  }
  return result;
}

std::string USBhost::getDeviceName() {
  usb_device_info_t info;
  esp_err_t err = usb_host_device_info(dev_hdl, &info);
  if (err != ESP_OK)
    return "";
  std::string manufacturer = usb_str_desc_to_string(info.str_desc_manufacturer);
  std::string product = usb_str_desc_to_string(info.str_desc_product);
  if (!product.empty() && !manufacturer.empty())
    return manufacturer + " - " + product;
  if (!product.empty())
    return product;
  if (!manufacturer.empty())
    return manufacturer;
  return "";
}

const usb_device_desc_t *USBhost::getDeviceDescriptor() {
  const usb_device_desc_t *device_desc;
  usb_host_get_device_descriptor(dev_hdl, &device_desc);
  return device_desc;
}

const usb_config_desc_t *USBhost::getConfigurationDescriptor() {
  const usb_config_desc_t *config_desc;
  usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
  return config_desc;
}

uint8_t USBhost::getConfiguration() { return getDeviceInfo().bConfigurationValue; }

usb_host_client_handle_t USBhost::clientHandle() { return client_hdl; }

usb_device_handle_t USBhost::deviceHandle() { return dev_hdl; }

#endif
