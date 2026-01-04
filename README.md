# 7i-sample.yaml — Build & Usage

## Purpose
Instructions for building, flashing and testing the ESPHome configuration file named `7i-sample.yaml`.

## Prerequisites
- Python 3 and pip OR Docker, or Home Assistant ESPHome add-on.
- ESPHome CLI: `pip install esphome`
- USB cable (for direct serial flashing) or network access for OTA flashing.
- Create `config/secrets.yaml` with your Wi‑Fi and OTA credentials:
```yaml
wifi_ssid: "SSID"
wifi_password: "pwd"
ota_password: "ota_pwd"
```

## Quick build (local CLI)
From the repository root (where `7i-sample.yaml` lives):
```bash
esphome compile 7i-sample.yaml
```
Firmware binaries will be produced in the `build/` directory.

## Flashing via USB (serial)
1. Connect the device via USB.  
2. Run:
```bash
esphome run 7i-sample.yaml
```
ESPhome will compile (if needed) and upload via serial automatically. To specify a serial port:
```bash
esphome run 7i-sample.yaml --device /dev/ttyUSB0
```

## Flashing via OTA
1. Ensure the YAML contains an `ota:` section and the device is on the same network.  
2. Upload using the device IP or let esphome detect it:
```bash
esphome upload 7i-sample.yaml --upload-port 192.168.1.50
# or
esphome run 7i-sample.yaml
```

## Using Docker
From the repo root:
```bash
docker run --rm -v "$(pwd)":/config -it esphome/esphome run 7i-sample.yaml
```
Replace `run` with `compile` if you only want to build.

## Home Assistant
Add `7i-sample.yaml` to the ESPHome add-on / dashboard and use the web UI to compile and upload firmware.

## Typical workflow notes
- Edit the YAML to set name, platform/board, Wi‑Fi, logger, api and ota sections.  
- Use `esphome compile` to verify configuration without flashing.  
- Use `esphome logs 7i-sample.yaml` to watch serial logs (or `esphome logs --device <port>`).

## Troubleshooting
- Permission errors on serial ports: add your user to `dialout`/`tty` groups or use `sudo`.  
- If OTA upload fails, verify device IP, API/OTA credentials and network firewall.  
- For build errors, run `esphome compile 7i-sample.yaml` and inspect the output.

## Tips
- Keep backups of working firmware binaries from `build/` if you need to revert.  
- Use `esphome dashboard .` to open a local web UI for managing multiple YAML files.

> Replace placeholders (Wi‑Fi credentials, device names, pins, etc.) in `7i-sample.yaml` before building to match your hardware and network environment. 7i-sample.yaml 

### Device screenshots

![About](images/about.png)
![Heating-Control](images/heating-control.png)

cd con  