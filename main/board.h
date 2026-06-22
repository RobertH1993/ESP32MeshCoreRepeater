#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Heltec WiFi LoRa 32 V4
 *
 * MCU:     ESP32-S3R2 (16 MB flash, 2 MB PSRAM)
 * Radio:   Semtech SX1262 via SPI
 * Display: 0.96" SSD1306 OLED (128x64, I2C) on stock boards
 *
 * Pin map aligned with MeshCore variants/heltec_v4 and the Heltec V4.2 datasheet.
 * https://github.com/meshcore-dev/MeshCore/tree/main/variants/heltec_v4
 * https://resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.2.0.pdf
 */

#define BOARD_NAME              "Heltec WiFi LoRa 32 V4"
#define BOARD_MCU               "ESP32-S3R2"
#define BOARD_RADIO             "SX1262"

/* -------------------------------------------------------------------------- */
/* LoRa radio — SPI (SX1262)                                                  */
/* -------------------------------------------------------------------------- */

#define P_LORA_NSS              8
#define P_LORA_SCK              9
#define P_LORA_MOSI             10
#define P_LORA_MISO             11
#define P_LORA_RESET            12
#define P_LORA_BUSY             13
#define P_LORA_DIO_1            14

/* ESP-IDF: use SPI2_HOST (FSPI) for the LoRa bus */
#define BOARD_LORA_SPI_HOST_NUM 2

/* SX1262 internal routing (not separate MCU GPIOs on this board) */
#define P_LORA_DIO2_AS_RF_SWITCH    1
#define P_LORA_DIO3_TCXO_VOLTAGE    1.8f

/* -------------------------------------------------------------------------- */
/* LoRa front-end module (FEM) / power amplifier                                */
/* -------------------------------------------------------------------------- */

/* VFEM LDO enable (must be HIGH for radio FEM power) */
#define P_LORA_PA_POWER         7

/*
 * GC1109 FEM (Heltec V4.0 / V4.2):
 *   CSD on GPIO2, CPS on GPIO46
 * KCT8103L FEM (Heltec V4.3+):
 *   CSD on GPIO2, CTX on GPIO5
 * Both FEM types share GPIO2 for chip-select / enable detection.
 */
#define P_LORA_GC1109_PA_EN     2   /* GC1109 CSD — chip enable, active HIGH */
#define P_LORA_GC1109_PA_TX_EN  46  /* GC1109 CPS — HIGH = TX PA, LOW = RX/bypass */

#define P_LORA_KCT8103L_PA_CSD  2   /* KCT8103L CSD — chip enable, active HIGH */
#define P_LORA_KCT8103L_PA_CTX  5   /* KCT8103L CTX — TX/RX path control (FEM pin) */

/* -------------------------------------------------------------------------- */
/* Status LED & user button                                                     */
/* -------------------------------------------------------------------------- */

#define P_LORA_TX_LED           35  /* Blue TX indicator LED */
#define PIN_USER_BTN            0   /* PRG button (active LOW) */

/* -------------------------------------------------------------------------- */
/* 0.96" OLED display (SSD1306, I2C) — default Heltec V4 board                  */
/* -------------------------------------------------------------------------- */

#define PIN_BOARD_SDA           17
#define PIN_BOARD_SCL           18
#define PIN_OLED_RESET          21

#define BOARD_OLED_I2C_ADDR     0x3C
#define BOARD_OLED_WIDTH        128
#define BOARD_OLED_HEIGHT       64

/* -------------------------------------------------------------------------- */
/* Power management & battery                                                   */
/* -------------------------------------------------------------------------- */

#define PIN_VEXT_EN             36  /* External 3.3 V rail (Vext) enable */
#define PIN_VEXT_EN_ACTIVE      0   /* LOW = Vext enabled */

#define PIN_ADC_CTRL            37  /* Battery voltage divider enable, active HIGH */
#define PIN_VBAT_READ           1   /* ADC1_CH0 — battery sense input */

/*
 * VBAT (mV) ≈ ADC_MULTIPLIER * 3300 * raw / 1024
 * Divider: 100 kΩ / 390 kΩ (see Heltec datasheet)
 */
#define BOARD_ADC_MULTIPLIER    5.42f

/* -------------------------------------------------------------------------- */
/* GNSS connector (SH1.25-8P)                                                   */
/* -------------------------------------------------------------------------- */

#define PIN_GPS_RX              38  /* ESP RX  ← GNSS TX */
#define PIN_GPS_TX              39  /* ESP TX  → GNSS RX */
#define PIN_GPS_RESET           42
#define PIN_GPS_RESET_ACTIVE    0   /* LOW = reset asserted */
#define PIN_GPS_EN              34
#define PIN_GPS_EN_ACTIVE       0   /* LOW = GNSS module enabled */

#define PIN_GPS_UART_NUM        UART_NUM_1

/* -------------------------------------------------------------------------- */
/* USB (native ESP32-S3)                                                        */
/* -------------------------------------------------------------------------- */

#define PIN_USB_D_MINUS         19
#define PIN_USB_D_PLUS          20

/* -------------------------------------------------------------------------- */
/* Expansion headers (J2 / J3)                                                  */
/* -------------------------------------------------------------------------- */

#define PIN_HEADER_UART_RX      44  /* J2 pin 5 — U0RXD */
#define PIN_HEADER_UART_TX      43  /* J2 pin 6 — U0TXD */

#define PIN_HEADER_GPIO_2       2
#define PIN_HEADER_GPIO_3       3
#define PIN_HEADER_GPIO_4       4
#define PIN_HEADER_GPIO_5       5
#define PIN_HEADER_GPIO_6       6
#define PIN_HEADER_GPIO_7       7
#define PIN_HEADER_GPIO_33      33
#define PIN_HEADER_GPIO_34      34
#define PIN_HEADER_GPIO_40      40  /* GNSS wakeup */
#define PIN_HEADER_GPIO_41      41  /* GNSS PPS */
#define PIN_HEADER_GPIO_45      45
#define PIN_HEADER_GPIO_46      46
#define PIN_HEADER_GPIO_47      47
#define PIN_HEADER_GPIO_48      48

/* -------------------------------------------------------------------------- */
/* LoRa defaults (MeshCore heltec_v4)                                           */
/* -------------------------------------------------------------------------- */

#define BOARD_LORA_TX_POWER_DBM       10
#define BOARD_LORA_MAX_TX_POWER_DBM   22
#define BOARD_LORA_SX126X_CURRENT_MA  140

/* -------------------------------------------------------------------------- */
/* Optional 1.14" TFT variant (Heltec V4 + TFT shield)                          */
/* Define BOARD_HELTEC_V4_TFT before including board.h to use these pins.     */
/* -------------------------------------------------------------------------- */

#ifdef BOARD_HELTEC_V4_TFT

#define PIN_TFT_SDA             33
#define PIN_TFT_SCL             17
#define PIN_TFT_CS              15
#define PIN_TFT_DC              16
#define PIN_TFT_RST             18
#define PIN_TFT_LEDA_CTL        21  /* Backlight, active HIGH */
#define PIN_TFT_LEDA_CTL_ACTIVE 1

#undef PIN_BOARD_SDA
#undef PIN_BOARD_SCL
#define PIN_BOARD_SDA           4   /* Shared I2C for sensors/expansion */
#define PIN_BOARD_SCL           3

#endif /* BOARD_HELTEC_V4_TFT */

#ifdef __cplusplus
}
#endif
