# Weather popup build and flash instructions

These steps build and flash the weather-popup layout for the ESP32-P4
Waveshare panel.

## Prerequisites

- Python 3 with ESPHome installed:

  ```powershell
  python -m pip install esphome
  ```

- A configured `config\secrets.yaml`.
- Either a USB connection to the panel or its IP address for OTA flashing.

## Validate the configuration

From the repository root:

```powershell
Set-Location .\config
esphome config P4-10-sample2.yaml
```

The command should finish without a `Failed config` error.

## Compile

```powershell
esphome compile P4-10-sample2.yaml
```

Compilation can take several minutes on the first run while PlatformIO
downloads the ESP-IDF and library dependencies.


## Flash over Wi-Fi

After the device has a working firmware and is connected to Wi-Fi:

```powershell
esphome run P4-10-sample2.yaml --device OTA
```

For a build that has already been compiled, upload the binary directly:

```powershell
esphome upload P4-10-sample2.yaml --device OTA
```

The device can also be selected automatically:

```powershell
esphome run P4-10-sample2.yaml
```

## Verify the weather popup

1. Open the weather tile on the panel.
2. Confirm that the popup shows eight forecast columns.
3. Check the popup vertical order:
   - weekday and icon;
   - high temperature;
   - hourly temperature line chart;
   - low temperature;
   - precipitation bars;
   - precipitation amount;
   - precipitation probability.
4. Confirm that the close button dismisses the popup.
5. Request a screenshot from:

   ```text
   http://<panel-ip-address>/screenshot.png
   ```

   The captured image should include the popup overlay.

## Verify the normal weather tile

Check both a short tile and a tall tile after flashing:

- A tile that is two rows high or shorter must use the regular forecast-column
  layout. It must not show the large inline chart layout.
- A wide tile that is more than two rows high may use the inline forecast
  layout with the chart and precipitation section.
- In the regular forecast columns, the icon must not overlap the high or low
  temperature.
- Temperature colors should be visible in the forecast:
  - below 18°C: blue;
  - 18–24.9°C: white;
  - 25–30°C: yellow to orange;
  - above 30°C: red/orange.
- Confirm that the current weather temperature uses the same color scale.

Use forecast data containing values near the boundaries, such as 15°C, 19°C,
25°C, and 30°C, so the color transitions can be verified rather than testing
only white-range temperatures.

## Screenshot and regression checks

For each layout, capture a screenshot after the weather state has refreshed:

```text
http://<panel-ip-address>/screenshot.png
```

Verify that:

- the popup overlay is present in the screenshot;
- all eight popup forecast columns are visible;
- the icon and temperature rows are separated;
- colors are applied to both high and low values;
- a two-row wide tile does not contain the inline chart.

## Serial logs

To inspect runtime weather updates while testing:

```powershell
esphome logs P4-10-sample2.yaml --device COM10
```

Look for weather forecast update messages and any LVGL allocation or rendering
errors. The following messages are useful when checking forecast updates:

```text
Applying encoded weather forecast
forecast[0]
weather forecast entry
```

Warnings about an invalid or incomplete forecast entry indicate that the
source data should be checked before judging the layout.
