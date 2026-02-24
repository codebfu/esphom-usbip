#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <stdlib.h>
#include <string.h>

#include "esphome/core/log.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "usb/usb_host.h"
#include "usb_host.h"
#include "usbip_protocol.hpp"

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define __bswap_32(x) __builtin_bswap32(x)
#define __bswap_16(x) __builtin_bswap16(x)
#else
#define __bswap_32(x) (x)
#define __bswap_16(x) (x)
#endif

// commands
#define OP_REQ_DEVLIST bswap_constant_16(0x8005)
#define OP_REP_DEVLIST bswap_constant_16(0x0005)
#define OP_REQ_IMPORT bswap_constant_16(0x8003)
#define OP_REP_IMPORT bswap_constant_16(0x0003)

#define USBIP_CMD_SUBMIT bswap_constant_16(0x01)
#define USBIP_RET_SUBMIT bswap_constant_32(0x03)
#define USBIP_CMD_UNLINK bswap_constant_16(0x02)
#define USBIP_RET_UNLINK bswap_constant_32(0x04)

#define USBIP_VERSION bswap_constant_16(0x0111)
#define USB_LOW_SPEED bswap_constant_32(1)
#define USB_FULL_SPEED bswap_constant_32(2)

static usbip_import_t import_data = {};
static usbip_devlist_t devlist_data = {};
static uint32_t last_unlink = 0;

static esp_event_loop_handle_t loop_handle;
static SemaphoreHandle_t usb_sem;
static SemaphoreHandle_t usb_sem1;

#define USB_CTRL_RESP 0x1001
#define USB_EPx_RESP 0x1002

ESP_EVENT_DECLARE_BASE(USBIP_EVENT_BASE);
ESP_EVENT_DEFINE_BASE(USBIP_EVENT_BASE);

static const char *const TAG = "usbip";

#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

static std::unordered_set<uint32_t> seqnum_set;
static std::mutex seqnum_mutex;

static std::map<int, std::string> s_socket_to_busid;
static std::map<std::string, int> s_busid_attached_count;
static std::mutex s_socket_mutex;

static USBipApplication *s_app = nullptr;

static void usb_ctrl_cb(usb_transfer_t *transfer) {
  esp_event_post_to(loop_handle, USBIP_EVENT_BASE, USB_CTRL_RESP, (void *)&transfer,
                    sizeof(usb_transfer_t *), 10);
}

static void usb_read_cb(usb_transfer_t *transfer) {
  esp_event_post_to(loop_handle, USBIP_EVENT_BASE, USB_EPx_RESP, (void *)&transfer,
                    sizeof(usb_transfer_t *), 10);
}

static void handle_usb_transfer_response(USBipDevice *dev, usb_transfer_t *transfer, bool is_ctrl) {
  usbip_transfer_context_t *ctx = (usbip_transfer_context_t *)transfer->context;
  usbip_submit_t *req = ctx->req;
  int sock = ctx->socket;
  uint32_t seqnum = __bswap_32(req->header.seqnum);

  {
    std::lock_guard<std::mutex> lock(seqnum_mutex);
    if (seqnum_set.count(seqnum) != 0) {
      delete req;
      delete ctx;
      dev->deallocate(transfer);
      return;
    }
    seqnum_set.insert(seqnum);
    if (seqnum_set.size() >= USBIP_MAX_SEQNUM)
      seqnum_set.clear();
  }

  int data_offset = is_ctrl ? 8 : 0;
  int _len = (int)transfer->actual_num_bytes - data_offset;
  if ((is_ctrl && _len < 0) || (!is_ctrl && _len <= 0)) {
    delete req;
    delete ctx;
    dev->deallocate(transfer);
    return;
  }
  if (req->header.direction == 0) {
    _len = 0;
  }

  req->header.command = USBIP_RET_SUBMIT;
  req->header.devid = 0;
  req->header.direction = 0;
  req->header.ep = 0;
  req->status = 0;
  req->length = __bswap_32(_len);
  req->start_frame = 0;
  req->padding = 0;

  if (_len > 0) {
    size_t copy_len = (_len < (int)sizeof(req->transfer_buffer)) ? (size_t)_len : sizeof(req->transfer_buffer);
    memcpy(&req->transfer_buffer[0], transfer->data_buffer + data_offset, copy_len);
  }
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    _len = 0;
    req->length = 0;
    req->status = -ETIME;
    req->error_count = 1;
  }

  int to_write = USBIP_HEADER_SIZE + _len;
  send(sock, (void *)req, to_write, MSG_DONTWAIT);
  delete req;
  delete ctx;
  dev->deallocate(transfer);
}

static void _event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data) {
  switch (event_id) {
    case USB_CTRL_RESP:
      handle_usb_transfer_response((USBipDevice *)event_handler_arg, *(usb_transfer_t **)event_data,
                                   true);
      break;
    case USB_EPx_RESP:
      handle_usb_transfer_response((USBipDevice *)event_handler_arg, *(usb_transfer_t **)event_data,
                                   false);
      break;
    default:
      break;
  }
}

static void _event_handler1(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                            void *event_data) {
  switch (event_id) {
    case USBIP_CMD_SUBMIT: {
      USBipDevice *dev = (USBipDevice *)event_handler_arg;
      urb_data_t *data = (urb_data_t *)event_data;
      int socket = data->socket;
      uint8_t *rx_buffer = (uint8_t *)data->rx_buffer;
      int len = data->len;
      int start = 0;
      int _len = len;

      do {
        usbip_submit_t *_req = (usbip_submit_t *)(rx_buffer + start);
        usbip_submit_t *req = new (std::nothrow) usbip_submit_t();
        if (req == nullptr) {
          break;
        }
        int tl = 0;
        if (_req->header.direction == 0)
          tl = __bswap_32(_req->length);

        memcpy(req, _req, USBIP_HEADER_SIZE + tl);

        int tlen = 0;
        if (req->header.ep == 0) {
          tlen = dev->req_ctrl_xfer(req, socket);
        } else {
          tlen = dev->req_ep_xfer(req, socket);
        }
        start += USBIP_HEADER_SIZE + tlen;
        _len -= USBIP_HEADER_SIZE + tlen;
      } while (_len >= (int)USBIP_HEADER_SIZE);
      free(rx_buffer);
      break;
    }

    case USBIP_CMD_UNLINK: {
      usbip_unlink_evt_t *evt = (usbip_unlink_evt_t *)event_data;
      usbip_submit_t *req = evt->req;
      int sock = evt->socket;
      req->header.command = USBIP_RET_UNLINK;
      req->header.devid = 0;
      req->header.direction = 0;
      req->header.ep = 0;
      req->status = 0;
      int to_write = USBIP_HEADER_SIZE;
      send(sock, (void *)req, to_write, MSG_DONTWAIT);
      delete req;
      break;
    }

    default:
      break;
  }
}

static void _event_handler2(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                            void *event_data) {
  int sock = -1;
  char busid[32] = {};
  if (event_id == OP_REQ_IMPORT && event_data) {
    usbip_import_evt_t *import_evt = (usbip_import_evt_t *)event_data;
    sock = import_evt->socket;
    strncpy(busid, import_evt->busid, sizeof(busid) - 1);
    busid[sizeof(busid) - 1] = '\0';
    for (size_t i = strlen(busid); i > 0 && (busid[i - 1] == ' ' || busid[i - 1] == '\t'); i--)
      busid[i - 1] = '\0';
  } else if (event_data) {
    usbip_socket_evt_t *evt = (usbip_socket_evt_t *)event_data;
    sock = evt->socket;
  }
  if (sock < 0)
    return;

  switch (event_id) {
    case OP_REQ_DEVLIST: {
      int to_write = 0;
      if (devlist_data.request.version == 0) {
        to_write = 12;
        devlist_data.request.version = USBIP_VERSION;
        devlist_data.request.command = OP_REP_DEVLIST;
        devlist_data.request.status = 0;
        devlist_data.count = 0;
      } else {
        to_write = 0x0c + __bswap_32(devlist_data.count) * 0x138 + devlist_data.bNumInterfaces * 4;
      }
      send(sock, (void *)&devlist_data, to_write, MSG_DONTWAIT);
      break;
    }

    case OP_REQ_IMPORT: {
      int to_write = sizeof(usbip_import_t);
      send(sock, (void *)&import_data, to_write, MSG_DONTWAIT);
      if (busid[0] && s_app) {
        ESP_LOGI(TAG, "OP_REQ_IMPORT: client attach busid='%s' sock=%d", busid, sock);
        std::lock_guard<std::mutex> lock(s_socket_mutex);
        s_socket_to_busid[sock] = busid;
        s_busid_attached_count[busid]++;
        s_app->notify_device_attached(busid);
      }
      break;
    }
  }
}

USBipDevice::USBipDevice() {
  usb_sem = xSemaphoreCreateBinary();
  usb_sem1 = xSemaphoreCreateBinary();
  xSemaphoreGive(usb_sem);
  xSemaphoreGive(usb_sem1);

  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, USB_CTRL_RESP, _event_handler, this);
  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, USB_EPx_RESP, _event_handler, this);
  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, USBIP_CMD_SUBMIT, _event_handler1,
                                  this);
  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, USBIP_CMD_UNLINK, _event_handler1,
                                  this);
}

USBipDevice::~USBipDevice() {
  esp_event_handler_unregister_with(loop_handle, USBIP_EVENT_BASE, ESP_EVENT_ANY_ID, _event_handler);
  esp_event_handler_unregister_with(loop_handle, USBIP_EVENT_BASE, ESP_EVENT_ANY_ID, _event_handler1);
  memset(&import_data, 0, sizeof(usbip_import_t));
  memset(&devlist_data, 0, sizeof(usbip_devlist_t));
}

bool USBipDevice::init(USBhost *host, const char *busid) {
  _host = host;
  strncpy(m_busid, busid, sizeof(m_busid) - 1);
  m_busid[sizeof(m_busid) - 1] = '\0';

  USBhostDevice::init(1032);
  xfer_ctrl->callback = usb_ctrl_cb;

  config_desc = host->getConfigurationDescriptor();

  int offset = 0;
  for (size_t n = 0; n < config_desc->bNumInterfaces; n++) {
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
    const usb_ep_desc_t *ep = nullptr;

    for (size_t i = 0; i < intf->bNumEndpoints; i++) {
      int _offset = 0;
      ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &_offset);
      uint8_t adr = ep->bEndpointAddress;
      if (adr & 0x80) {
        endpoints[adr & 0xf][1] = ep;
      } else {
        endpoints[adr & 0xf][0] = ep;
      }
    }
    esp_err_t err =
        usb_host_interface_claim(_host->clientHandle(), _host->deviceHandle(), n, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "interface claim failed: %s (interface %d)", esp_err_to_name(err), (int)n);
      deinit();
      return false;
    }
  }

  fill_list_data();
  fill_import_data();
  return true;
}

void USBipDevice::fill_import_data() {
  usb_device_info_t info = _host->getDeviceInfo();
  const usb_device_desc_t *dev_desc = _host->getDeviceDescriptor();

  memset(&import_data, 0, sizeof(usbip_import_t));
  import_data.request.version = USBIP_VERSION;
  import_data.request.command = OP_REP_IMPORT;
  import_data.request.status = 0;
  strcpy(import_data.path, "/espressif/usbip/usb1");
  strncpy(import_data.busid, m_busid, sizeof(import_data.busid) - 1);
  import_data.busid[sizeof(import_data.busid) - 1] = '\0';
  import_data.busnum = __bswap_32(1);
  import_data.devnum = __bswap_32(1);

  import_data.speed = info.speed ? __bswap_32(2) : __bswap_32(1);
  devlist_data.idVendor = __bswap_16(dev_desc->idVendor);
  devlist_data.idProduct = __bswap_16(dev_desc->idProduct);
  devlist_data.bcdDevice = __bswap_16(dev_desc->bcdDevice);
  import_data.idVendor = __bswap_16(dev_desc->idVendor);
  import_data.idProduct = __bswap_16(dev_desc->idProduct);
  import_data.bcdDevice = __bswap_16(dev_desc->bcdDevice);
  import_data.bDeviceClass = dev_desc->bDeviceClass;
  import_data.bDeviceSubClass = dev_desc->bDeviceSubClass;
  import_data.bDeviceProtocol = dev_desc->bDeviceProtocol;
  import_data.bConfigurationValue = config_desc->bConfigurationValue;
  import_data.bNumConfigurations = dev_desc->bNumConfigurations;
  import_data.bNumInterfaces = config_desc->bNumInterfaces;
}

void USBipDevice::fill_list_data() {
  usb_device_info_t info = _host->getDeviceInfo();
  const usb_device_desc_t *dev_desc = _host->getDeviceDescriptor();

  int offset = 0;
  for (size_t n = 0; n < config_desc->bNumInterfaces; n++) {
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
    devlist_data.intfs[n].bInterfaceClass = intf->bInterfaceClass;
    devlist_data.intfs[n].bInterfaceSubClass = intf->bInterfaceSubClass;
    devlist_data.intfs[n].bInterfaceProtocol = intf->bInterfaceProtocol;
    devlist_data.intfs[n].padding = 0;
  }

  devlist_data.request.version = USBIP_VERSION;
  devlist_data.request.command = OP_REP_DEVLIST;
  devlist_data.request.status = 0;
  devlist_data.busnum = __bswap_32(1);
  devlist_data.devnum = __bswap_32(1);
  devlist_data.count = __bswap_32(1);
  strcpy(devlist_data.path, "/espressif/usbip/usb1");
  strncpy(devlist_data.busid, m_busid, sizeof(devlist_data.busid) - 1);
  devlist_data.busid[sizeof(devlist_data.busid) - 1] = '\0';

  devlist_data.speed = info.speed ? USB_FULL_SPEED : USB_LOW_SPEED;
  devlist_data.idVendor = __bswap_16(dev_desc->idVendor);
  devlist_data.idProduct = __bswap_16(dev_desc->idProduct);
  devlist_data.bcdDevice = __bswap_16(dev_desc->bcdDevice);
  devlist_data.bDeviceClass = dev_desc->bDeviceClass;
  devlist_data.bDeviceSubClass = dev_desc->bDeviceSubClass;
  devlist_data.bDeviceProtocol = dev_desc->bDeviceProtocol;
  devlist_data.bConfigurationValue = config_desc->bConfigurationValue;
  devlist_data.bNumConfigurations = dev_desc->bNumConfigurations;
  devlist_data.bNumInterfaces = config_desc->bNumInterfaces;
}

int USBipDevice::req_ctrl_xfer(usbip_submit_t *req, int socket) {
  usb_transfer_t *_xfer_ctrl = allocate(1000);
  if (_xfer_ctrl == NULL) {
    delete req;
    return 0;
  }
  usbip_transfer_context_t *ctx = new (std::nothrow) usbip_transfer_context_t{req, socket};
  if (ctx == nullptr) {
    deallocate(_xfer_ctrl);
    delete req;
    return 0;
  }
  _xfer_ctrl->callback = usb_ctrl_cb;
  _xfer_ctrl->context = ctx;
  _xfer_ctrl->bEndpointAddress =
      __bswap_32(req->header.ep) | (__bswap_32(req->header.direction) << 7);

  usb_setup_packet_t *temp = (usb_setup_packet_t *)_xfer_ctrl->data_buffer;
  size_t n = 0;
  if (req->header.direction == 0) {
    n = __bswap_32(req->length);
    memcpy(_xfer_ctrl->data_buffer, (void *)&req->transfer_buffer, n);
  }
  memcpy(temp->val, (uint8_t *)&req->setup, 8 + n);
  _xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t) + __bswap_32(req->length);
  _xfer_ctrl->bEndpointAddress =
      __bswap_32(req->header.ep) | (__bswap_32(req->header.direction) << 7);

  esp_err_t err = usb_host_transfer_submit_control(_host->clientHandle(), _xfer_ctrl);
  if (err != ESP_OK) {
    deallocate(_xfer_ctrl);
    delete req;
    delete ctx;
    return 0;
  }
  return n;
}

int USBipDevice::req_ep_xfer(usbip_submit_t *req, int socket) {
  size_t _len = __bswap_32(req->length);
  uint16_t mps = 64;

  if (req->header.direction != 0) {
    uint8_t adr = __bswap_32(req->header.ep);
    const usb_ep_desc_t *ep = endpoints[adr][1];
    if (ep) {
      mps = ep->wMaxPacketSize;
    } else {
      return 0;
    }
    _len = usb_round_up_to_mps(_len, mps);
  }

  usb_transfer_t *xfer_read = allocate(_len);
  if (xfer_read == NULL)
    return 0;
  usbip_transfer_context_t *ctx = new (std::nothrow) usbip_transfer_context_t{req, socket};
  if (ctx == nullptr) {
    deallocate(xfer_read);
    delete req;
    return 0;
  }
  xfer_read->callback = &usb_read_cb;
  xfer_read->context = ctx;
  xfer_read->bEndpointAddress = __bswap_32(req->header.ep);

  int n = 0;
  if (req->header.direction == 0) {
    memcpy(xfer_read->data_buffer, (void *)&req->transfer_buffer, _len);
    n = _len;
  }

  xfer_read->num_bytes = _len;
  xfer_read->bEndpointAddress =
      __bswap_32(req->header.ep) | (__bswap_32(req->header.direction) << 7);

  esp_err_t err = usb_host_transfer_submit(xfer_read);
  if (err != ESP_OK) {
    deallocate(xfer_read);
    delete req;
    delete ctx;
    return 0;
  }
  return n;
}

void parse_request(const int sock, uint8_t *rx_buffer, size_t len) {
  uint32_t cmd = ((usbip_request_t *)rx_buffer)->command;

  switch (cmd) {
    case OP_REQ_DEVLIST: {
      usbip_socket_evt_t evt_data = {sock};
      esp_event_post_to(loop_handle, USBIP_EVENT_BASE, OP_REQ_DEVLIST, &evt_data,
                        sizeof(evt_data), 10);
      break;
    }
    case OP_REQ_IMPORT: {
      usbip_import_evt_t evt_data = {sock, {}};
      if (len >= (int)sizeof(usbip_import_t)) {
        const usbip_import_t *req = (const usbip_import_t *)rx_buffer;
        strncpy(evt_data.busid, req->busid, sizeof(evt_data.busid) - 1);
        evt_data.busid[sizeof(evt_data.busid) - 1] = '\0';
      }
      esp_event_post_to(loop_handle, USBIP_EVENT_BASE, OP_REQ_IMPORT, &evt_data,
                        sizeof(evt_data), 10);
      break;
    }
    case USBIP_CMD_SUBMIT: {
      uint8_t *buf = (uint8_t *)malloc(len);
      if (buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer for USBIP_CMD_SUBMIT");
        break;
      }
      memcpy(buf, rx_buffer, len);
      urb_data_t data = {
          .socket = sock,
          .len = (int)len,
          .rx_buffer = buf,
      };
      esp_event_post_to(loop_handle, USBIP_EVENT_BASE, USBIP_CMD_SUBMIT, &data,
                        sizeof(urb_data_t), 10);
      break;
    }
    case USBIP_CMD_UNLINK: {
      usbip_submit_t *req = new (std::nothrow) usbip_submit_t();
      if (req == nullptr) {
        break;
      }
      last_unlink = __bswap_32(((usbip_submit_t *)rx_buffer)->flags);
      {
        std::lock_guard<std::mutex> lock(seqnum_mutex);
        seqnum_set.insert(last_unlink);
        if (seqnum_set.size() >= USBIP_MAX_SEQNUM)
          seqnum_set.clear();
      }
      memcpy(req, rx_buffer, USBIP_HEADER_SIZE);
      usbip_unlink_evt_t evt_data = {req, sock};
      esp_event_post_to(loop_handle, USBIP_EVENT_BASE, USBIP_CMD_UNLINK, &evt_data,
                        sizeof(evt_data), 10);
      break;
    }
    default:
      ESP_LOGE(TAG, "unknown command: %" PRIu32, cmd);
      break;
  }
}

USBIP::USBIP() {
  esp_event_loop_args_t loop_args = {
      .queue_size = 100,
      .task_name = "usbip_events",
      .task_priority = 21,
      .task_stack_size = 4 * 1024,
      .task_core_id = 0,
  };

  esp_event_loop_create(&loop_args, &loop_handle);

  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, OP_REQ_DEVLIST, _event_handler2,
                                  NULL);
  esp_event_handler_register_with(loop_handle, USBIP_EVENT_BASE, OP_REQ_IMPORT, _event_handler2,
                                  NULL);
}

USBIP::~USBIP() {}

USBipApplication::USBipApplication() : m_host(nullptr), m_device(nullptr), m_ready(false) {}

USBipApplication::~USBipApplication() {
  s_app = nullptr;
  if (m_device) {
    m_device->deinit();
    delete m_device;
    m_device = nullptr;
  }
  if (m_host) {
    delete m_host;
    m_host = nullptr;
  }
}

void USBipApplication::clientEventCallback(const usb_host_client_event_msg_t *event_msg, void *arg) {
  if (!s_app)
    return;
  USBipApplication *app = s_app;

  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    app->m_host->open(event_msg);
    usb_device_info_t info = app->m_host->getDeviceInfo();
    const usb_device_desc_t *dev_desc = app->m_host->getDeviceDescriptor();

    snprintf(app->m_busid, sizeof(app->m_busid), "1-%d", event_msg->new_dev.address);
    app->m_device = new (std::nothrow) USBipDevice();
    if (app->m_device && app->m_device->init(app->m_host, app->m_busid)) {
      app->m_ready = true;
      if (app->m_on_connected) {
        std::string name = app->m_host->getDeviceName();
        app->m_on_connected(app->m_busid, dev_desc->idVendor, dev_desc->idProduct, name.c_str());
      }
    } else {
      if (app->m_device) {
        delete app->m_device;
        app->m_device = nullptr;
      }
    }
  } else {
    if (app->m_on_disconnected && app->m_busid[0]) {
      app->m_on_disconnected(app->m_busid);
      app->m_busid[0] = '\0';
    }
    app->m_ready = false;
    if (app->m_device) {
      app->m_device->deinit();
      delete app->m_device;
      app->m_device = nullptr;
    }
  }
}

void USBipApplication::init(bool enable_hubs) {
  s_app = this;
  m_host = new (std::nothrow) USBhost();
  if (!m_host) {
    return;
  }
  m_host->registerClientCb(clientEventCallback);
  m_host->init(enable_hubs);
}

void USBipApplication::on_socket_close(int sock) {
  if (!s_app)
    return;
  std::string busid;
  {
    std::lock_guard<std::mutex> lock(s_socket_mutex);
    auto it = s_socket_to_busid.find(sock);
    if (it != s_socket_to_busid.end()) {
      busid = it->second;
      s_socket_to_busid.erase(it);
      auto cit = s_busid_attached_count.find(busid);
      if (cit != s_busid_attached_count.end()) {
        cit->second--;
        if (cit->second <= 0) {
          s_busid_attached_count.erase(cit);
          ESP_LOGI(TAG, "Socket close: client release busid='%s' sock=%d", busid.c_str(), sock);
          if (m_on_released) {
            m_on_released(busid.c_str());
          }
        }
      }
    }
  }
}

void usbip_on_socket_close(int sock) {
  if (s_app) {
    s_app->on_socket_close(sock);
  }
}

void USBipApplication::notify_device_attached(const char *busid) {
  if (m_on_attached) {
    m_on_attached(busid);
  }
}

void USBipApplication::refresh_device_list(void (*cb)(const char *busid, uint16_t vid, uint16_t pid,
                                                      const char *name, void *arg),
                                           void *arg,
                                           int *out_num_on_bus) {
  if (!m_host || !m_host->client_hdl)
    return;

  uint8_t dev_addr_list[16];
  int num_dev = 0;
  esp_err_t err =
      usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_dev);
  if (err != ESP_OK)
    return;

  if (out_num_on_bus)
    *out_num_on_bus = num_dev;

  if (m_ready && m_device && m_host && cb) {
    const usb_device_desc_t *desc = m_host->getDeviceDescriptor();
    if (desc) {
      std::string name = m_host->getDeviceName();
      cb(m_busid, desc->idVendor, desc->idProduct, name.c_str(), arg);
    }
  }
}
