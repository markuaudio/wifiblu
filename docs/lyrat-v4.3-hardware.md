# ESP32-LyraT v4.3 hardware and setup notes

Initial audit: 2026-10-25. Later in this session, ESP-IDF v6.1 was installed and a minimal firmware build passed; see 01-esp-idf-setup-hinglish.md. Nothing has been flashed.

## Official references

- Schematic (2022-01-19): https://dl.espressif.com/dl/schematics/ESP32-LYRAT_V4.3-20220119.pdf
- Hardware reference: https://docs.espressif.com/projects/esp-adf/en/latest/multimedia-boards/dev-boards/board-esp32-lyrat-v4.3.html

These describe the official board. Confirm the actual module marking and board revision before assuming memory size or component population.

## Firmware approach

Use ESP-IDF for our own firmware. Configure board pins explicitly and implement codec configuration ourselves. Begin with serial logging, then GPIO, I2C codec access, I2S playback, and microphone capture. No ESP-ADF board helpers are planned initially.

## Signal paths

- Control: ESP32 -> I2C -> ES8388 configuration registers.
- Playback: ESP32 -> I2S -> ES8388 DAC -> headphones / NS4150 speaker amplifiers.
- Capture: onboard microphones -> ES8388 ADC -> I2S -> ESP32.
- Programming: PC -> USB-UART connector -> CP2102N -> ESP32 UART0.
- USB power and USB-UART are separate connectors.

## GPIO mapping

| Function | ESP32 GPIO | Notes |
| --- | --- | --- |
| Codec I2C SDA | 18 | Control bus |
| Codec I2C SCL | 23 | Control bus |
| I2S MCLK | 0 | Also boot/download strap; account for upload jumpers |
| I2S BCLK / SCLK | 5 | Audio bit clock |
| I2S WS / LRCK | 25 | Audio frame clock |
| ESP32 audio TX -> codec DSDIN | 26 | Playback |
| Codec ASDOUT -> ESP32 audio RX | 35 | Recording; ESP32 input-only GPIO |
| Speaker PA enable | 21 | Separate from codec initialization |
| Headphone insertion detect | 19 | Software handles output selection |
| Green user LED | 22 | Separate from power/charging indicators |
| REC button | 36 / SENSOR_VP | Input-only |
| MODE button | 39 / SENSOR_VN | Input-only |
| SET touch | 32 | Touch input |
| PLAY touch | 33 | Touch input |
| VOL+ touch | 27 | Touch input |
| VOL- touch | 13 | Shared with SD D3 / JTAG |
| SD CLK / CMD / D0 | 14 / 15 / 2 | 1-bit SD signals |
| SD D1 / D2 / D3 | 4 / 12 / 13 | Additional 4-bit signals |
| SD card detect | 34 | Input-only |

## Shared-pin constraints

- GPIO12-15 are shared among SD, JTAG and other functions. Set DIP switches for the selected use before testing.
- SD 1-bit configuration in the reference: DIP 1-7 OFF (8 not applicable).
- SD 4-bit configuration: DIP 1-2 ON, 3-7 OFF. VOL- touch is then unavailable.
- JTAG configuration: DIP 1-2 OFF, 3-7 ON. Remove the SD card; VOL- and AUX detection are unavailable.
- GPIO12 AUX detection is controlled through DIP 7. The reference warns an inserted AUX plug at power-on can prevent boot with this option enabled.
- Automatic download uses EN, GPIO0 and GPIO2 through JP23/JP24/JP25. The reference instructs removing all three jumpers after upload. GPIO0 is also the audio master clock.
- Manual download sequence: hold BOOT, press/release RESET, then release BOOT.

## PC setup audit

The following records the initial state before installation; the setup guide has the completed installation results.

- Workspace was empty; no ESP-IDF project or workspace configuration existed.
- VS Code, Git and Python 3.14 executables are on PATH.
- Installed VS Code extension list does not include Espressif ESP-IDF or PlatformIO.
- idf.py, CMake, Ninja and xtensa-esp32-elf-gcc are not on the current shell PATH.
- No IDF/ADF-related environment variables were found.
- Standard locations C:\Espressif and %USERPROFILE%\.espressif were not found. A custom SDK location has not been ruled out.
- Windows Win32_SerialPort query returned no devices. Board connection, cable and driver remain unverified.
- Conclusion: this VS Code/workspace is not currently verified ready to build ESP32 firmware. Install/configure ESP-IDF, then prove the setup with a minimal build before flashing.
