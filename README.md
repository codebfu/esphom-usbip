# ESPHome USBIP Component

ESPHome external component that exposes USB devices connected to an ESP32-S3 over TCP/IP using the [USB/IP](https://www.kernel.org/doc/html/latest/usb/usbip.html) protocol.

## Requirements

- **ESP32-S3** or **ESP32-S2** (USB Host support)
- USB device connected to the board's USB port

## Installation

### Local (development)

```yaml
external_components:
  - source:
      type: local
      path: /path/to/esp32-usbip-esphome/components
    components: [usbip]
```

### GitHub

```yaml
external_components:
  - source: github://codebfu/esphom-usbip
    components: [usbip]
```

## Configuration

```yaml
usbip:
  id: usbip_main
  port: 3240                    # TCP port (default: 3240)
  keepalive_idle: 5             # TCP keepalive idle time in seconds (default: 5)
  keepalive_interval: 5         # Keepalive probe interval (default: 5)
  keepalive_count: 3            # Number of keepalive probes (default: 3)
  enable_hubs: false            # Set true for USB hub / multi-device support (default: false)
  device_count_id: usb_device_count   # Optional: sensor ID for device count
  sensors:                      # Optional: auto-update sensors per device index
    - connected_id: usb_dev0_connected
      inuse_id: usb_dev0_inuse
      pid_id: usb_dev0_vidpid   # VID:PID as "0x1234:0x5678"
      busid_id: usb_dev0_busid
      name_id: usb_dev0_name
  ignore_vid_pid:               # Optional: ignore devices by VID:PID
    - "0x0781:*"               # Use "*" to ignore all PIDs for that VID
  ignore_busid:                 # Optional: ignore devices by bus ID
    - "1-1"
```

## API Functions

The component exposes getter functions callable from lambdas (e.g. in template sensors). Use `id(usbip_main)` to reference the component.

### By device index (0-based)

| Function | Return | Description |
|----------|--------|-------------|
| `get_device_vid_pid(index)` | string | VID:PID as "0x1234:0x5678" |
| `get_device_busid(index)` | string | Bus ID (e.g. "1-1") |
| `get_device_connected(index)` | bool | Device is physically connected |
| `get_device_inuse(index)` | bool | Device is attached to a usbip client |
| `get_device_count()` | int | Number of devices |
| `get_device_name(index)` | string | Device name |

### By vid:pid (returns first match)

| Function | Return | Description |
|----------|--------|-------------|
| `get_busid_by_vid_pid(vid, pid)` | string | Bus ID for given VID:PID |
| `get_name_by_vid_pid(vid, pid)` | string | Device name |
| `get_connected_by_vid_pid(vid, pid)` | bool | Connected state |
| `get_inuse_by_vid_pid(vid, pid)` | bool | In-use state |

### By busid

| Function | Return | Description |
|----------|--------|-------------|
| `get_device_vid_pid_by_busid(busid)` | string | VID:PID as "0x1234:0x5678" |
| `get_device_name_by_busid(busid)` | string | Device name |
| `get_device_connected_by_busid(busid)` | bool | Connected state |
| `get_device_inuse_by_busid(busid)` | bool | In-use state |

### Example: template sensors

```yaml
text_sensor:
  - platform: template
    name: "USB Device 0 VID:PID"
    lambda: return { id(usbip_main).get_device_vid_pid(0) };
    update_interval: 5s

  - platform: template
    name: "USB Device 0 BusID"
    lambda: return { id(usbip_main).get_device_busid(0) };
    update_interval: 5s

binary_sensor:
  - platform: template
    name: "USB Device 0 Connected"
    lambda: return id(usbip_main).get_device_connected(0);
    update_interval: 5s

  - platform: template
    name: "USB Device 0 In Use"
    lambda: return id(usbip_main).get_device_inuse(0);
    update_interval: 5s
```

### Example: lookup by VID:PID

```yaml
text_sensor:
  - platform: template
    name: "SanDisk BusID"
    lambda: return { id(usbip_main).get_busid_by_vid_pid(0x0781, 0x5580) };
    update_interval: 5s
```

### Example: lookup by busid

```yaml
text_sensor:
  - platform: template
    name: "Device 1-1 VID:PID"
    lambda: return { id(usbip_main).get_device_vid_pid_by_busid("1-1") };
    update_interval: 5s
```

## Usage

1. Flash the firmware to your ESP32-S3
2. Connect a USB device to the board
3. Device connection and disconnection is detected automatically. A manual `refresh_devices()` (e.g. via a button) is normally not needed; when called, the list of known devices is logged at INFO level.
4. On your Linux host, run:

```bash
# List available devices
usbip list -r <device-ip>

# Attach a device (replace 1-1 with the busid from list)
sudo usbip attach -r <device-ip> -b 1-1
```

## Migration to Standalone usb_host

To use both `usb_uart` (CDC-ACM, CH340, etc.) and USBIP on the same device, see [docs/migration_to_standalone_usb_host.md](docs/migration_to_standalone_usb_host.md).
