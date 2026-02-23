#pragma once

#include <string.h>

#include "esp_event.h"
#include "usb/usb_host.h"
#include "usb_device.h"

/* USBIP protocol constants */
#define USBIP_HEADER_SIZE 0x30
#define USBIP_MAX_SEQNUM 999
#define USBIP_TRANSFER_BUFFER_SIZE 1024

/* Swap bytes in 16-bit value.  */
#define bswap_constant_16(x) (((uint16_t)((((x) >> 8) & 0xff) | (((x) & 0xff) << 8))))

/* Swap bytes in 32-bit value.  */
#define bswap_constant_32(x) \
  ((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >> 8) | (((x) & 0x0000ff00u) << 8) | \
   (((x) & 0x000000ffu) << 24))

typedef struct {
  uint16_t version;
  uint16_t command;
  uint32_t status;
} usbip_request_t;

typedef struct {
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t padding;
} usbip_interface_t;

typedef struct {
  usbip_request_t request;
  uint32_t count;
  char path[256];
  char busid[32];
  uint32_t busnum;
  uint32_t devnum;
  uint32_t speed;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bConfigurationValue;
  uint8_t bNumConfigurations;
  uint8_t bNumInterfaces;
  usbip_interface_t intfs[10];
} usbip_devlist_t;

typedef struct {
  usbip_request_t request;
  char path[256];
  char busid[32];
  uint32_t busnum;
  uint32_t devnum;
  uint32_t speed;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bConfigurationValue;
  uint8_t bNumConfigurations;
  uint8_t bNumInterfaces;
} usbip_import_t;

typedef struct {
  uint32_t command;
  uint32_t seqnum;
  uint32_t devid;
  uint32_t direction;
  uint32_t ep;
} usbip_header_basic_t;

typedef struct {
  usbip_header_basic_t header;
  union {
    uint32_t flags;
    uint32_t status;
  };
  uint32_t length;
  uint32_t start_frame;
  uint32_t num_packets;
  union {
    uint32_t interval;
    uint32_t error_count;
  };
  union {
    uint64_t setup;
    uint64_t padding;
  };
  uint8_t transfer_buffer[USBIP_TRANSFER_BUFFER_SIZE];
} __attribute__((__packed__)) usbip_submit_t;

typedef struct {
  usbip_header_basic_t header;
  union {
    int32_t unlink_seqnum;
    int32_t status;
  };
  uint8_t padding[24];
} __attribute__((__packed__)) usbip_unlink_t;

typedef struct {
  int socket;
  int len;
  uint8_t *rx_buffer;
} urb_data_t;

typedef struct {
  usbip_submit_t *req;
  int socket;
} usbip_transfer_context_t;

typedef struct {
  int socket;
} usbip_socket_evt_t;

typedef struct {
  int socket;
  char busid[32];
} usbip_import_evt_t;

typedef struct {
  usbip_submit_t *req;
  int socket;
} usbip_unlink_evt_t;

class USBhost;

class USBipDevice : public USBhostDevice {
 private:
  const usb_ep_desc_t *ep_out;
  const usb_ep_desc_t *endpoints[15][2];
  const usb_config_desc_t *config_desc;

 public:
  USBipDevice();
  ~USBipDevice();
  bool init(USBhost *);

  int req_ctrl_xfer(usbip_submit_t *req, int socket);
  int req_ep_xfer(usbip_submit_t *req, int socket);

 private:
  void fill_import_data();
  void fill_list_data();
};

class USBIP {
 public:
  USBIP();
  ~USBIP();
};

typedef void (*usbip_device_connected_cb_t)(const char *busid, uint16_t vid, uint16_t pid,
                                            const char *name);
typedef void (*usbip_device_disconnected_cb_t)(const char *busid);
typedef void (*usbip_device_attached_cb_t)(const char *busid);
typedef void (*usbip_device_released_cb_t)(const char *busid);

class USBipApplication {
 public:
  USBipApplication();
  ~USBipApplication();
  void init(bool enable_hubs = false);
  bool isReady() const { return m_ready; }

  void set_callbacks(usbip_device_connected_cb_t on_connected,
                     usbip_device_disconnected_cb_t on_disconnected,
                     usbip_device_attached_cb_t on_attached,
                     usbip_device_released_cb_t on_released) {
    m_on_connected = on_connected;
    m_on_disconnected = on_disconnected;
    m_on_attached = on_attached;
    m_on_released = on_released;
  }

  void on_socket_close(int sock);
  void notify_device_attached(const char *busid);

  void refresh_device_list(void (*cb)(const char *busid, uint16_t vid, uint16_t pid,
                                     const char *name, void *arg),
                           void *arg,
                           int *out_num_on_bus = nullptr);

 private:
  static void clientEventCallback(const usb_host_client_event_msg_t *event_msg, void *arg);

  USBIP m_usbip;
  USBhost *m_host;
  USBipDevice *m_device;
  bool m_ready;
  char m_busid[32] = {};

  usbip_device_connected_cb_t m_on_connected = nullptr;
  usbip_device_disconnected_cb_t m_on_disconnected = nullptr;
  usbip_device_attached_cb_t m_on_attached = nullptr;
  usbip_device_released_cb_t m_on_released = nullptr;
};

void usbip_on_socket_close(int sock);

void parse_request(const int sock, uint8_t *rx_buffer, size_t len);
