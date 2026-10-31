#pragma once

// ESP32-LyraT v4.3 wiring. Same board as the earlier kanchan speaker project.
// Only this file describes the hardware; a new board needs a new mapping.
#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 23
#define BOARD_CODEC_ADDR 0x10 // ES8388, 7-bit address
#define BOARD_I2S_MCLK 0
#define BOARD_I2S_BCLK 5
#define BOARD_I2S_LRCK 25
#define BOARD_I2S_DOUT 26 // ESP32 -> ES8388 DSDIN (playback)
#define BOARD_I2S_DIN 35  // ES8388 ASDOUT -> ESP32 (microphones; input-only pin)
#define BOARD_PA_ENABLE 21
#define BOARD_LED 22
#define BOARD_TOUCH_PLAY 8  // GPIO33
#define BOARD_TOUCH_SET 9   // GPIO32
#define BOARD_TOUCH_UP 7    // GPIO27
#define BOARD_TOUCH_DOWN 4  // GPIO13; DIP 1-7 OFF, no SD/JTAG.

// Bluetooth identity. Phones that still remember the old music-only "kanchan"
// must forget it once and pair again so they re-read the new service list (HFP).
#define BT_DEVICE_NAME "wifi blu"
#define BT_LEGACY_PIN "0000" // Old (pre-2.1) phones ask for this PIN.

// Music (A2DP) path.
#define MUSIC_BUFFER_BYTES (32 * 1024)
#define MUSIC_PREFILL_BYTES 8192
#define MUSIC_BOOT_VOLUME 70       // Phone absolute volume replaces this after connect.
// Music peak ceiling: -6 dBFS. The power self-test below plays a -6 dBFS tone,
// so with this ceiling no music can draw more amplifier current than the test
// did. The limiter (fast attack, 150 ms release) holds peaks at this level.
#define MUSIC_PCM_PEAK_LIMIT 16384

// ES8388 DAC digital volume profile (see dac_profiles[] in es8388.c). P2 = -9 dB
// is only the level used until the power self-test has run; the test result
// (stored in NVS) replaces it. P4 = -6 dB is ESP-ADF's LyraT maximum and the
// highest level the self-test tries.
#define AUDIO_BOOT_DAC_PROFILE 2
#define CAL_MAX_PROFILE 4
// Power self-test (first boot, and after console '0' or 'k'): a CAL_TONE_HZ
// tone at CAL_TONE_AMPLITUDE (-3 dBFS: 3 dB above the music peak ceiling, as
// margin) plays through both speakers for CAL_STEP_MS at each DAC profile from
// P0 upwards, while the Bluetooth radio runs an inquiry so the ESP32 side draws
// about what it draws when streaming (verified: a test with the radio idle
// passed P4 while streaming at P4 still reset the board).
// The step in progress is written to NVS first; if the supply collapses and
// the IMP809 supervisor resets the board, the next boot knows which step did
// it and saves the previous one as the limit. No phone, no disconnect involved.
#define CAL_TONE_HZ 60
#define CAL_TONE_AMPLITUDE 23170
#define CAL_STEP_MS 1500
// Margin: the saved result is (highest step that passed) minus this. Test
// T12 (docs/08): the test passed P4 but real streaming at P4 still reset the
// board once; P3 then ran for 8+ minutes at full phone volume without a reset.
#define CAL_MARGIN_STEPS 1
// Brownout guard (power_guard.c): comparator level 7 = 2.80 V on the ESP32.
// On an event the ISR cuts the amplifier; the audio task keeps it off for
// POWER_GUARD_HOLDOFF_MS, lowers the DAC profile by POWER_GUARD_DAC_STEP (saved
// in NVS, at most once per POWER_GUARD_MIN_INTERVAL_MS) and re-arms the guard.
#define POWER_GUARD_BOD_LEVEL 7
#define POWER_GUARD_HOLDOFF_MS 400
#define POWER_GUARD_DAC_STEP 1
#define POWER_GUARD_MIN_INTERVAL_MS 2000

// Call (HFP) path. Voice PCM arrives from the phone as 16-bit mono at 8 kHz
// (CVSD) or 16 kHz (mSBC); the I2S bus runs at that same rate during a call.
#define CALL_BOOT_VOLUME 10          // HFP speaker gain 0..15; phone sends +VGS to change it.
#define CALL_RX_BUFFER_BYTES 6144    // phone -> speaker PCM ring (8 kHz: ~380 ms)
#define CALL_TX_BUFFER_BYTES 6144    // mic -> phone PCM ring
#define CALL_PREFILL_BYTES 480       // 30 ms at 8 kHz before the speaker starts
#define MIC_PGA_REG 0x88             // ES8388 reg 0x09: +24 dB on both onboard mics
#define MIC_DIGITAL_GAIN 2.0f        // software gain on mic PCM before it goes to the phone
#define CALL_RING_ON_MS 1000         // local ring tone pattern when the phone sends no in-band ring
#define CALL_RING_OFF_MS 2000

// Link manager: how hard we try to connect by ourselves (older phones often
// wait for the accessory to open the profiles). The speaker never gives up:
// LINK_RECONNECT_ATTEMPTS quick attempts, then one every LINK_RECONNECT_SLOW_PERIOD_S
// until the phone answers or the user disconnects (SET hold / 'x').
// Our own paging blocks the radio's inquiry/page scans, so new phones cannot
// see or reach the speaker while it pages: keep the duty cycle low.
#define LINK_RECONNECT_ATTEMPTS 4
#define LINK_RECONNECT_PERIOD_S 10
#define LINK_RECONNECT_SLOW_PERIOD_S 45
#define LINK_PAGE_TIMEOUT 0x2000 // 5.12 s per attempt (controller default); 0x4000 = 10 s
#define LINK_PROFILE_FOLLOWUP_S 4 // after one profile (or pairing) is up, open the other one ourselves

// Wi-Fi (Phase 1). Station mode on the home network in normal use; the setup
// hotspot (SoftAP + captive portal) runs only while no network is saved, after
// a SET hold of WIFI_SETUP_HOLD_S seconds, or console 'W', and closes itself
// after WIFI_SETUP_TIMEOUT_S or once the device has joined a network.
// Coexistence rule (docs/00-wifi-options.md): SoftAP next to Bluetooth is
// unstable, station mode is fine, heavy traffic during a call hurts the call.
#define WIFI_HOSTNAME "wifiblu"          // http://wifiblu.local/
#define WIFI_SETUP_SSID "wifi blu setup"
#define WIFI_SETUP_PASSWORD ""           // "" = open hotspot; 8+ characters = WPA2
#define WIFI_SETUP_TIMEOUT_S 600
#define WIFI_SETUP_HOLD_S 6              // SET pad hold (2 s = disconnect still fires first)
#define WIFI_RECONNECT_PERIOD_S 10       // station: retry the saved network forever
// Wi-Fi transmit power cap in 0.25 dB units (esp_wifi_set_max_tx_power): 34 = 8.5 dBm,
// 44 = 11 dBm, 78 = 19.5 dBm (chip default). The LyraT 5 V -> 3.3 V path sags on
// 20 dBm bursts while Bluetooth is up (A/B 2026-10-30: BT-only v1.0.1 runs clean on
// the same supply, BT + Wi-Fi at full power browns out every 0.4 s and resets in
// 12-21 s; 34 and 44 both ran clean for 60-120 s). 44 keeps some range for the
// station link to a home router; the phone is next to the speaker during setup.
#define WIFI_MAX_TX_POWER_QDBM 44
#define OTA_VALID_AFTER_S 45             // new image confirms itself once Bluetooth is ready and this time passed
