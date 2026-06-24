# MeshCore Repeater — Heltec WiFi LoRa 32 V4

ESP-IDF firmware for the **Heltec WiFi LoRa 32 V4** board, implementing a
[MeshCore](https://github.com/meshcore-dev/MeshCore) repeater node designed to
run on solar power with minimal energy consumption.

---

## Hardware

| Component | Part |
|---|---|
| MCU | ESP32-S3R2 (16 MB flash, 2 MB PSRAM) |
| Radio | Semtech SX1262 (LoRa, SPI) |
| Display | 0.96" SSD1306 OLED 128×64 (I2C) |
| FEM | GC1109 / KCT8103L front-end module |
| Band | EU868 (configurable) |

Pin definitions are in [`main/board.h`](main/board.h).

---

## Features

### Radio
- Interrupt-driven RX via DIO1 — the CPU does not poll the radio.
- Continuous receive mode using `SX126X_RX_CONTINUOUS` via the Semtech SX126x driver.
- DMA-backed SPI transfers for the radio HAL.
- TCXO and DCDC regulator enabled for the Heltec V4 hardware.
- Configurable band, spreading factor, bandwidth, and coding rate.

### Power management
- ESP32-S3 dynamic frequency scaling (40–160 MHz).
- Light sleep between packets: the radio wakes the CPU via DIO1 GPIO interrupt,
  allowing the ESP32-S3 to sleep during idle periods.
- Intended for solar-powered operation; the node can sustain itself on a modest
  panel and small LiPo cell.

### MeshCore protocol
- Full wire-format packet parser (`main/meshcore/meshcore_packet.c`):
  - Header decoding (route type, payload type, payload version).
  - Optional transport codes.
  - Path length / hop count decoding (1–3 byte path hashes).
  - Per-type payload parsing: `ADVERT`, `ACK`, `GRP_TXT`, `GRP_DATA`,
    `ANON_REQ`, `CONTROL`, `REQ`, `RESPONSE`, `PATH`, `TXT_MSG`, and raw types.
- Node advertisement parsing: public key, timestamp, Ed25519 signature,
  optional GPS coordinates and node name.
- Group channel decryption: AES-128-ECB with HMAC-SHA256 MAC verification
  (channel key derived from channel name via SHA-256).

### Security
- **Flood protection**: packets are identified by a hash; already-forwarded
  packets are not retransmitted (MeshCore `hasSeen` table pattern).
- **HMAC-MAC verification**: every group message is checked against a 2-byte
  HMAC-SHA256 truncation before the payload is accepted or forwarded.
- **Replay resistance**: advertisement packets carry an Ed25519 signature over
  public key + timestamp + app data; forged or replayed adverts are dropped.
- **Minimal attack surface**: the node does not expose a web interface, BLE, or
  Wi-Fi. USB is used for flashing and serial logging only.

### Storage
- NVS (Non-Volatile Storage) initialised at boot with automatic erase-and-reformat
  on partition corruption or version mismatch.
- Contact and channel tables are defined in `main/meshcore/mesh.h` and intended
  to be persisted as NVS blobs.

---

## Project structure

```
├── CMakeLists.txt
├── main/
│   ├── board.h                  Pin definitions — Heltec V4
│   ├── hello_world_main.c       Application entry point
│   ├── heltecv4.c / .h          Board-specific SX126x setup (TCXO, FEM, DCDC)
│   ├── lora.c / .h              LoRa radio init, RX/TX tasks
│   ├── sx126x_hal.c / .h        ESP-IDF SPI HAL for the Semtech driver
│   ├── display.c / .h           SSD1306 OLED initialisation
│   ├── sx126x_driver/           Semtech SX126x vendor driver (submodule)
│   └── meshcore/
│       ├── mesh.h               Contact and channel table types
│       ├── meshcore_packet.h    MeshCore packet + payload structs and API
│       └── meshcore_packet.c    Wire-format parser implementation
└── README.md
```

---

## Building

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/).

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

---

## Configuration

Edit `main/hello_world_main.c` to change the LoRa band and modulation
parameters passed to `lora_init_radio()`:

```c
lora_init_radio(&lora_radio,
    LORA_BAND_EU868,
    SX126X_LORA_SF7,
    SX126X_LORA_BW_062,
    SX126X_LORA_CR_4_5);
```

Channel keys for group message decryption are derived from the channel name
at runtime using SHA-256 and stored in the `meshcore_channel_t` table.

---

## Status

Work in progress. The radio receive path, packet parser, and repeating of flood messages is supported.

---

## References

- [MeshCore protocol documentation](https://github.com/meshcore-dev/MeshCore/tree/main/docs)
- [Heltec V4 datasheet](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.2.0.pdf)
- [Semtech SX1262 datasheet](https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R000000HSSc/p7S.OQKR0dHFWJrFzxm5N.Gl_3r_0nxKQdEBgRLpblY)
- [MeshCore cryptography](https://jacksbrain.com/2026/01/a-hitchhiker-s-guide-to-meshcore-cryptography/)
