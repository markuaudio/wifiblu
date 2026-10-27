# BOM: bill of materials for the bleaudiocall project

Two parts: (A) what to buy to build the project (this is the real shopping list), (B) what is on the LyraT V4.3 board itself (from schematic Rev 4.3, 2022-01-19; for understanding/repairing your board).

## A. Shopping list (project level)

| # | Item | Qty | Spec / note | Why |
|---|---|---|---|---|
| 1 | **ESP32-LyraT V4.3** dev board | 1 | Espressif official; module ESP32-WROVER-E (or the WROOM-32E variant). Our board: ESP32-D0WD-V3 chip, **8 MB flash** (measured with esptool) | All the audio hardware is on it: codec, 2 PAs, 2 mics, touch pads, USB-UART, charger |
| 2 | Speaker | 2 | **4 Ω, 3 W**, JST PH 2.0 mm 2-pin plug (or Dupont female wires temporarily) | NS4150 PA is meant for a 4 Ω load; 8 Ω also works (less power) |
| 3 | Micro USB cable | 2 | USB-A → Micro-B, data capable, short and thick (low resistance) | One for the POWER port, one for the UART port; both are plugged in at the same time |
| 4 | 5 V supply | 1 | **5 V, ≥ 2 A** charger (a PC USB port also works but may trip on bass, see kanchan docs 08/11) | 2 × 3 W speakers + ESP32 radio |
| 5 | Jumper caps 2.54 mm | 3 | For JP23/JP24/JP25 (shipped with the board) | Automatic download; remove after the upload |
| 6 | PC | 1 | Windows 10/11 (this project), 10 GB free, VS Code | ESP-IDF v6.1 toolchain installed by EIM into a path without spaces |
| 7 | Phone | 1 | Any Bluetooth phone: new (SSP) or old (PIN) | A2DP music + HFP call tests |
| 8 | Optional: Li-ion battery | 1 | Single cell 3.7 V, 2-pin JST PH 2.0 (polarity printed on the board) | Charged by the AP5056; keeps USB noise away |
| 9 | Optional: headphones | 1 | 3.5 mm, **OMTP** standard (not CTIA) | Headphone path test (no auto-switch in this firmware) |
| 10 | Optional: microSD | 1 | FAT32 | Not used by this firmware; keep DIP 1-7 OFF |

No soldering. No extra ICs. The firmware is in this folder.

## B. Board-level BOM (from the LyraT V4.3 schematic, verified)

### B1. ICs / modules

| Ref | Part | Function | Relation to our firmware |
|---|---|---|---|
| U6 | **ESP32-WROVER-E** | MCU + Wi-Fi/BT module, 4 MB flash + 8 MB PSRAM (official spec); U13 = ESP32-WROOM-32E footprint, NC (alternate module) | `sdkconfig`: 8 MB flash (measured), PSRAM not used |
| U5 | **ES8388** (Everest) | Stereo audio codec: 2 ADC, 2 DAC, mic PGA, headphone amp; I2C address 7-bit 0x10 (8-bit 0x20) | `es8388.c`: all registers; I2C GPIO18/23; I2S GPIO0/5/25/26/35 |
| U11, U12 | **NS4150** ×2 | Mono class-D PA, 3 W, differential input IN+/IN-, CTRL pin = IO21 (shared by both) | `BOARD_PA_ENABLE 21`; the LOUT2/ROUT2 -45 dB cancellation fix exists because of this differential input |
| U1 | **CP2102N-A01-GQFN28** (Silicon Labs) | USB-UART bridge, auto-download via DTR/RTS | COM6, 115200 baud, `idf.py flash` |
| U14 | **AP5056** | Li-ion linear charger (CC/CV), R5 1 kΩ 1% = charge current setting, CHRG/STDBY LEDs D2/D4 | nothing to do with the firmware |
| U3 | **LD1117S33CTR** | 3.3 V LDO (800 mA) → VDD33 (ESP32 module + digital) | brownout detector kept on |
| U8 | **LP2985A-33DBVT** | 3.3 V LDO 150 mA → AU_VCC33 / Codec_3V3 (ES8388 analog), fed through ferrite L9 from VBAT_For_Audio | |
| U9 | **LP2985A-33DBVT** | 3.3 V LDO 150 mA → SDIO_3V3 (microSD) | SD not used |
| U10 | **IMP809REUR/T (2.63 V)** | Voltage supervisor → IMP_RST → EN | the board resets itself when 3.3 V drops (part of the kanchan reset loop) |
| U15 | **2N7002DW** | Dual N-MOSFET, EN/reset logic | |
| Q9, Q10, Q11 | **L8050QLT1G** NPN ×3 | Automatic download: DTR/RTS → EN_Auto, IO0_Auto, IO2_Auto (through jumpers JP23/24/25 to EN/IO0/IO2) | without the jumpers: `No serial data received` (docs 00/04) |
| Q1, Q5, Q6, Q7, Q8 | **LBSS138LT1G** N-MOSFET ×5 | Q6 green LED driver (IO22), Q1 Boot, Q5/Q8 AUX detect, Q7 IO2 | `BOARD_LED 22` |
| Q2 | **AO3401** P-MOSFET | USB vs battery power switch (VBUS → VCC) | |
| D3 | 1N5819 Schottky | VUSB from the USB-UART port | |
| D2, D8 | RED LED | D2 = charging, D8 = power on (VBAT_For_LDO) | |
| D4, D17 | GREEN LED | D4 = standby (USB power), **D17 = user LED (IO22 via Q6, default off)** | link task LED |
| D9–D14 | LESD8D3.3CAT5G ×6 | ESD, SD card lines | |
| D19–D22 | LESD5D5.0CT1G ×4 | ESD, USB D+/D-/VBUS | |

### B2. Connectors / mechanical

| Ref | Part | Function |
|---|---|---|
| J1 | USB_CON8 Micro-USB | **USB-UART** (logs/flash from the PC); also VUSB power |
| J17 | USB_CON8 Micro-USB | **USB POWER** (main 5 V) |
| J3 | SS-12D00 slide switch | Power ON/OFF |
| J2 | CON2 (JST PH 2.0) | Battery |
| J15 | SPEAKER (JST PH 2.0) | **LEFT** speaker (PA_OUTL+/−), 4 Ω |
| J16 | SPEAKER (JST PH 2.0) | **RIGHT** speaker (PA_OUTR+/−), 4 Ω |
| J12 | AUDIO_OUT 3.5 mm | Headphones (LOUT1/ROUT1 via 22 µF, detect on IO19) |
| J14 | AUX_IN 3.5 mm | Line in → LIN2/RIN2, AUX_Detect; do not plug in at power-on |
| J4, J9 | MIC ×2 | Onboard electret mics → LIN1/RIN1 (MICBIAS from the codec) |
| J5 | TF_CARD | microSD, ESD D9–D14 |
| J18 | KE DSHP08TSGER 8-way DIP | Function switch: GPIO12/13/14/15 → SD or JTAG (all OFF = this project) |
| J19 | CON4X1 2.54 | JTAG: MTDO(15) MTCK(13) MTDI(12) MTMS(14) |
| J20 | CON4X1 2.54 | UART: 3V3 TXD0 RXD0 GND |
| J21 | CON6X1 2.54 | I2S: MCLK(0) SCLK(5) LRCK(25) DSDIN(26) ASDOUT(35) GND |
| J22 | CON3X1 2.54 | I2C: SCL(23) SDA(18) GND |
| J23, J24, J25 | CON2X1 2.54 + jumper cap | **Auto download**: EN_Auto–EN, IO0_Auto–IO0, IO2_Auto–IO2 |
| SW1, SW2 | SW_R tact | **Boot** (IO0), **RST** (EN) |
| SW3, SW4 | SW_R tact | **Rec** (SENSOR_VP/IO36), **Mode** (SENSOR_VN/IO39) |
| TP3–TP6 | Touch_Pad | **Play IO33, Set IO32, Vol- IO13, Vol+ IO27** (510 Ω series R63/R68/R75/R79) |
| H1–H4 | M3-L5-3.5 | Mounting holes |
| L8, L14 | BLM18PG181SN1D ferrite ×2 | PA supply (VBAT_For_Audio → PA_PVDD1/2) |
| L9–L13 | BLM18SG121TN1D 3 A ferrite ×5 | VCC_5V → VBAT_For_LDO / For_Audio / For_SDIO; audio LDO input |

### B3. Audio signal path components (the ones that decide the firmware's numbers)

| Net | Parts | Meaning |
|---|---|---|
| I2S lines IO0/5/25/26/35 → codec | R55/R57/R58/R60/R61 = 33 Ω series | signal integrity; ESP32 master, codec slave (`reg 0x08 = 0x00`) |
| I2C SCL/SDA | R47/R49 = 100 Ω + C32/C33 = 20 pF ("RC to avoid errors during I2C read/write") | 100 kHz I2C is enough |
| LOUT1/ROUT1 → headphones + PA IN+ | C29/C30 = 22 µF, R50/R54 = 33 Ω; PA side R151/R240 = **30 kΩ** + C17/C222 0.1 µF | NS4150 gain = 300 k (internal) / 30 k = 10 V/V = **20 dB** |
| LOUT2/ROUT2 → PA IN− | C218/C219 = 22 µF, R237/R238 = 33 Ω; R149/R239 = **30 kΩ** + C16/C221 0.1 µF | This is why LOUT2 must be kept at -45 dB (reg 0x30/0x31 = 0x00): otherwise IN+ and IN− carry the same signal and cancel |
| PA outputs | C93/C94/C96/C97 = 330 pF, R243/R244/R246/R247 = 0 Ω, C42/C95 390 pF NC | class-D EMI filter |
| PA CTRL (IO21) | R144 0 Ω, R142/R235 = 10 kΩ pull-down, R143/R236 10 kΩ NC | PA defaults to OFF = `gpio_set_level(21, 0)` at boot |
| Mic bias / input | R65/R76 1 kΩ + R67/R78 2 kΩ divider from AU_VCC33, C41 1 µF → LIN1, C50 1 µF **NC** → RIN1 | **The RIN1 coupling cap is NC**: the right mic is unpopulated by default on the schematic. The firmware averages (L+R)/2; if RIN1 is silent, raise `MIC_DIGITAL_GAIN` or use L only |
| Codec references | C38/C39, C43/C44, C46/C47 = 10 µF + 0.1 µF on VMID / ADCVREF / VREF | "no vias, same layer" note |
| Headphone detect | IO19 ← R250 22.1 kΩ pull-up, jack switch | not used by this firmware |
| AUX detect | IO12 ← Q5/Q8, DIP 7 | an AUX plug at power-on can block boot (IO12 strap) |

### B4. Passives summary (schematic count)

0 Ω (35, jumpers/options), 0.1 µF 16 V (23), 10 kΩ (20 + 8 NC), 10 µF 16 V (18), 1 kΩ (14), 1 µF 16 V (10), 33 Ω (9), 22 µF 10 V (6), 2 kΩ (6), 20 pF (5), 100 Ω (5), 330 pF (4), 39 pF (4), 510 Ω (4), 30 kΩ (4), 100 µF 6.3 V (2), 10 nF (2), 100 kΩ (3), 22.1 kΩ (2), 47.5 kΩ (1), 4.7 kΩ (2), 4.7 Ω (1), 1 µF 25 V (2). Full list: `lyrat-v4.3-bom-from-schematic.csv`.

### B5. Power tree (from the schematic)

```
USB POWER J17 (VBUS) ──┐
USB-UART J1 ─ D3 ─ VUSB┤── Q2 AO3401 / R214 ── VCC ── J3 switch ── VCC_5V
Battery J2 ─ AP5056 ───┘                                       │
                        ┌──────────────────────┬───────────────┴───────────────┐
                 L11 VBAT_For_LDO       L10 VBAT_For_Audio               L12 VBAT_For_SDIO
                 U3 LD1117 3.3 V        ├─ L8/L14 → NS4150 PVDD (PA)       U9 LP2985 3.3 V
                 VDD33 → ESP32, digital └─ L9 → U8 LP2985 3.3 V → codec    SDIO_3V3
```

The PA runs straight from the 5 V rail (VBAT_For_Audio): that is why the PC USB port's current limit reset the whole board on bass peaks (kanchan docs 10/11). Use a 5 V 2 A charger.
