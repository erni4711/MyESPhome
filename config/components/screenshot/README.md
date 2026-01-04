# Screenshot component for ESPHome

A small ESPHome custom component that captures a display or camera framebuffer and saves it as an image file or serves it via the device web server. Useful for debugging displays, generating thumbnails, or exposing device screen state to home automation.

## Authors
This component is written with the support of ChatGPT (GPT-5 mini)
PNG output ist based on https://github.com/bitbank2/PNGenc.git


## Features
Captures a screenshot on http://<ipadress>/screenshot.png

## Requirements
- ESPHome with custom component support
- LVGL 

## Installation
Place the component folder in your ESPHome config directory under `components/screenshot/` (source files, headers and this README).

## Configuration example
Add the component to your device YAML and adjust options as needed:


# Example display this YAML

see 7i-sample.yaml

## License
Apache — see LICENSE file for details.