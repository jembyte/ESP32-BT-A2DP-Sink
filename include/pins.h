#pragma once

// I2S -> external DAC (e.g. PCM5102A).
// Prefixed CUSTOM_: arduino-audio-tools already #defines plain PIN_I2S_* for
// its own default board pinout, which would collide with a same-named const.
const int CUSTOM_PIN_I2S_MCK = -1;
const int CUSTOM_PIN_I2S_BCK = 26;
const int CUSTOM_PIN_I2S_WS = 25;
const int CUSTOM_PIN_I2S_DATA_OUT = 27;
const int CUSTOM_PIN_I2S_DATA_IN = -1;

// Rotary encoder. CLK/DT/SW sit on input-only pins (34-39): ESP32 has no
// internal pull-up there, but the encoder module already carries its own.
//const uint8_t PIN_ENCODER_CLK = 36;
//const uint8_t PIN_ENCODER_DT = 34;
const uint8_t PIN_ENCODER_SW = 39;

// 16x2 character LCD on a PCF8574 I2C backpack
const uint8_t PIN_LCD_SDA = 21;
const uint8_t PIN_LCD_SCL = 22;
const uint8_t LCD_I2C_ADDR = 0x27; // or 0x3F
const uint32_t LCD_I2C_FREQ = 400000;
const uint8_t LCD_COLS = 16;
const uint8_t LCD_ROWS = 2;
