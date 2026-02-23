# Migration from Option A to Option B: Standalone usb_host Component

This document describes how to migrate from the integrated usbip component (Option A) to a standalone `usb_host` component (Option B), enabling both `usb_uart` and USBIP on the same ESP32-S3 device.

## Context

**Why migrate?** Option B allows using ESPHome's `usb_uart` (e.g., for CDC-ACM, CH340 devices) alongside USBIP on the same hardware. A single USB Host driver serves both use cases.

**Prerequisites:**
- ESPHome with built-in `usb_host` support (ESP32-S3, ESP32-S2, or ESP32-P4)
- Familiarity with ESPHome external components

## Technical Steps

### 1. Extract usb_host to a standalone component

Move `components/usbip/usb_host/` to `components/usb_host/` at the repository root.

### 2. Replace with ESPHome usb_host and add extensions

- Clone or download the official ESPHome `usb_host` component from [github.com/esphome/esphome](https://github.com/esphome/esphome)
- Replace the extracted code with the ESPHome version
- Add the following extensions to `USBClient`:
  - `bool claim_interface(uint8_t interface_num)` → calls `usb_host_interface_claim`
  - `void release_interface(uint8_t interface_num)` → calls `usb_host_interface_release`
  - `usb_device_handle_t get_device_handle()` (and `get_client_handle()` if needed) for raw transfers

### 3. Create USBIPClient

Create a `USBIPClient` subclass of `USBClient` (or use a device with `vid=0x0000`, `pid=0x0000` as wildcard) that:
- Overrides `on_connected()` to claim all interfaces
- Uses `transfer_in`, `transfer_out`, `control_transfer` for USBIP transfers

### 4. Update the usbip component

- Remove the internal `usb_host/` subfolder from `components/usbip/`
- Add `DEPENDENCIES = ["usb_host"]` in `usbip/__init__.py`
- Import USB host types from `esphome.components.usb_host` instead of local headers

### 5. Update example.yaml

```yaml
external_components:
  - source:
      type: local
      path: /path/to/esp32-usbip-esphome/components
    components: [usb_host, usbip]   # usb_host replaces the built-in

usb_host:
  enable_hubs: false
  max_transfer_requests: 16
  devices:
    - id: usbip_client
      vid: "0x0000"
      pid: "0x0000"    # USBIP: wildcard for any device

usbip:
  port: 3240
```

## Validation Checklist

- [ ] Build completes without errors
- [ ] USBIP works: `usbip list -r <device-ip>` shows the device
- [ ] usb_uart works: CDC-ACM or CH340 device is recognized and usable

## References

- [ESPHome External Components](https://esphome.io/components/external_components/)
- [ESPHome usb_host documentation](https://esphome.io/components/usb_host.html)
- Plan section 9: Migration Option A → Option B
