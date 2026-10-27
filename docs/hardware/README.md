# docs/hardware: ESP32-LyraT V4.3 schematic, layout, BOM

This folder holds the board's official Espressif design data so that whoever builds this project does not have to search for it elsewhere. Download date: 2026-10-27.

| File | What it is | Source (Espressif) |
|---|---|---|
| `espressif-downloads/ESP32-LYRAT_V4.3-20220119-schematic.pdf` | **Official schematic**, Rev 4.3, dated Wednesday, January 19, 2022, 1 sheet, size D. This is the version linked from the ESP-ADF hardware reference page. | https://dl.espressif.com/dl/schematics/ESP32-LYRAT_V4.3-20220119.pdf |
| `espressif-downloads/esp32-lyrat-v4.3-schematic-older.pdf` | Older V4.3 schematic upload (same board, earlier export). For reference only; use the newer one. | https://dl.espressif.com/dl/schematics/esp32-lyrat-v4.3-schematic.pdf |
| `espressif-downloads/ESP32-LyraT_v4.3_component_layout.pdf` | Component layout: page 1 top view (all designators, header pin names, 95.5 × 80.6 mm), page 2 bottom view. | https://dl.espressif.com/dl/schematics/ESP32-LyraT_v4.3_component_layout.pdf |
| `schematic-pages/*.png` | Rendered pages of the PDFs above (schematic at 200 dpi full page + 6 tiles at 170 dpi, layout at 150 dpi). For viewing without a PDF reader. | generated locally (PyMuPDF) |
| `schematic-pages/*.txt` | Text layer of the PDFs (designators, values, net names). | generated locally |
| `lyrat-v4.3-bom-from-schematic.csv` | Designator → value/part extracted from the schematic text layer (290 rows). `?` = the value was not found next to the designator in the text layer (pin number or separate line). | generated locally |
| `BOM.md` | Curated BOM: what to buy for the project + the board's main ICs/connectors/passives, verified against the schematic tiles. | this folder |

Online references (not downloaded, the links are enough):

- Hardware reference page (block diagram, pin table, DIP table): https://docs.espressif.com/projects/esp-adf/en/latest/multimedia-boards/dev-boards/board-esp32-lyrat-v4.3.html
- Getting started (what you need, jumpers, first power-on): https://docs.espressif.com/projects/esp-adf/en/latest/multimedia-boards/dev-boards/get-started-esp32-lyrat.html
- ES8388 datasheet (Everest): http://www.everest-semi.com/pdf/ES8388%20DS.pdf (mirror that worked: https://www.pawpaw.cn/media/documents/2021-12/ES8388_DS.pdf)
- ESP32-WROVER-E datasheet: https://www.espressif.com/sites/default/files/documentation/esp32-wrover-e_esp32-wrover-ie_datasheet_en.pdf
- ESP32 datasheet: https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- Espressif audio hardware design guidelines: https://www.espressif.com/sites/default/files/documentation/esp32_audio_design_guidelines__en.pdf
- ES8388 reference driver (ESP-ADF): `../references/es8388-reference.c.txt` (license included)

Note: Espressif marks the board as "Proprietary"; the schematic is publicly available for reading, learning and writing your own firmware. PCB Gerber/Altium files are not public, so "printing your own board" is not part of this guide; buy a ready-made LyraT V4.3 board.
