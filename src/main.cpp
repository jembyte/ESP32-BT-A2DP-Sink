/**
 * @file main.cpp
 * @brief A2DP sink to I2S DAC, with a 16x2 I2C LCD and an encoder push button.
 * Derived from basic-a2dp-audioi2s.ino.
 *
 * @author Phil Schatzmann
 * @copyright GPLv3
 */

#define USE_LEGACY_I2S false

#include "AudioTools.h"
#include "BluetoothA2DPSink.h" // https://github.com/pschatzmann/ESP32-A2DP

#include <OneButton.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

#include "lcd_text.h"
#include "pins.h"

static const char* BT_DEVICE_NAME = "BT AUDIO";

BluetoothA2DPSink a2dp_sink;
I2SStream i2s;

// ---- UI tuning ----
static const uint32_t BLINK_MS = 700;        // "connect phone" blink half-period
static const uint32_t WAVE_MS = 100;         // animation frame time
static const uint32_t META_SETTLE_MS = 250;  // wait for the last metadata field
static const bool TRACK_CENTERED = true;     // center artist/title when they fit
static const char PROMPT[] = "connect phone";
static const size_t TXT_MAX = 128;

static_assert(LCD_ROWS == 2, "UI is laid out for a 2-row LCD");

static hd44780_I2Cexp lcd(LCD_I2C_ADDR);
static OneButton btn;
static lcdtext::Marquee<LCD_COLS> mq[LCD_ROWS];
static bool lcdOk;
static char lcdFrame[LCD_ROWS][LCD_COLS];  // wanted content
static char lcdShown[LCD_ROWS][LCD_COLS];  // content currently on the LCD

enum Screen : uint8_t { SCR_NONE, SCR_IDLE, SCR_WAIT, SCR_TRACK };
static Screen screen = SCR_NONE;
static volatile bool playing;  // play/pause intent, flipped by the button at once
static bool hasMeta;
static bool blinkOn;
static uint32_t blinkAt, waveAt;
static float wavePhase;
static char nameTxt[TXT_MAX], artistTxt[TXT_MAX], titleTxt[TXT_MAX];  // LCD-safe

// ---- Metadata: written by the BT task, consumed by loop() ----
static portMUX_TYPE metaMux = portMUX_INITIALIZER_UNLOCKED;
static char metaArtist[TXT_MAX], metaTitle[TXT_MAX];
static volatile uint32_t metaVer, metaStamp;
static uint32_t metaSeen;

static void onMetadata(uint8_t id, const uint8_t* text) {
  char* dst = id == ESP_AVRC_MD_ATTR_ARTIST  ? metaArtist
              : id == ESP_AVRC_MD_ATTR_TITLE ? metaTitle
                                             : nullptr;
  if (!dst) return;
  const uint32_t now = millis();
  portENTER_CRITICAL(&metaMux);
  strlcpy(dst, (const char*)text, TXT_MAX);
  metaStamp = now;
  metaVer = metaVer + 1;
  portEXIT_CRITICAL(&metaMux);
}

static void clearMeta() {
  portENTER_CRITICAL(&metaMux);
  metaArtist[0] = metaTitle[0] = 0;
  metaVer = metaVer + 1;
  metaSeen = metaVer;
  portEXIT_CRITICAL(&metaMux);
  artistTxt[0] = titleTxt[0] = 0;
  hasMeta = false;
}

// Picks up new metadata once the fields stopped arriving (no half-updated lines).
static void syncMeta(uint32_t now) {
  if (metaVer == metaSeen || (int32_t)(now - metaStamp) < (int32_t)META_SETTLE_MS) return;
  char a[TXT_MAX], t[TXT_MAX];
  portENTER_CRITICAL(&metaMux);
  metaSeen = metaVer;
  strlcpy(a, metaArtist, sizeof a);
  strlcpy(t, metaTitle, sizeof t);
  portEXIT_CRITICAL(&metaMux);

  lcdtext::sanitize(a, a, sizeof a);
  lcdtext::sanitize(t, t, sizeof t);
  if (strcmp(a, artistTxt)) {
    strcpy(artistTxt, a);
    if (screen == SCR_TRACK) mq[0].set(artistTxt, TRACK_CENTERED, now);
  }
  if (strcmp(t, titleTxt)) {
    strcpy(titleTxt, t);
    if (screen == SCR_TRACK) mq[1].set(titleTxt, TRACK_CENTERED, now);
  }
  hasMeta = artistTxt[0] || titleTxt[0];
}

// ---- Play/pause ----
// Acts on the local intent, never on the phone's AVRCP reports (they lag).
// play()/pause() block for 100 ms (key press, wait, release), so they run in
// their own task. Otherwise loop() could not see a quick second tap meanwhile.
static TaskHandle_t cmdTask;

static void cmdLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // taps queued meanwhile: last intent wins
    if (playing) a2dp_sink.play();
    else a2dp_sink.pause();
  }
}

static void onPress() {
  if (!cmdTask || !a2dp_sink.is_avrc_connected()) return;
  playing = !playing;
  xTaskNotifyGive(cmdTask);
}

// Resyncs the intent when the audio stream changes on its own (phone side).
static void syncPlayState(bool conn) {
  static bool wasStarted;
  const bool started = conn && a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
  if (started != wasStarted) {
    wasStarted = started;
    playing = started;
  }
}

// ---- LCD ----
static void loadGlyphs() {  // CGRAM 0..7: vertical bars, 1..8 pixel rows high
  uint8_t g[8];
  for (uint8_t lv = 0; lv < 8; lv++) {
    for (uint8_t r = 0; r < 8; r++) g[r] = r >= 7 - lv ? 0x1F : 0;
    lcd.createChar(lv, g);
  }
}

// Writes only the span that changed on each row.
static void flush() {
  if (!lcdOk) return;
  for (uint8_t r = 0; r < LCD_ROWS; r++) {
    uint8_t a = 0, b = LCD_COLS;
    while (a < b && lcdFrame[r][a] == lcdShown[r][a]) a++;
    while (b > a && lcdFrame[r][b - 1] == lcdShown[r][b - 1]) b--;
    if (a == b) continue;
    lcd.setCursor(a, r);
    lcd.write((const uint8_t*)&lcdFrame[r][a], b - a);
    memcpy(&lcdShown[r][a], &lcdFrame[r][a], b - a);
  }
}

// Two blended sine waves drawn as bars 1..16 pixel rows high over both lines.
static void drawWave(uint32_t now) {
  if ((int32_t)(now - waveAt) < 0) return;
  waveAt = now + WAVE_MS;
  wavePhase += 0.4f;
  for (uint8_t c = 0; c < LCD_COLS; c++) {
    const float v = 0.5f * (sinf(wavePhase + c * 0.6f) + sinf(wavePhase * 0.7f - c * 0.35f));
    const int h = 1 + (int)((v + 1.0f) * 7.5f + 0.5f);
    lcdFrame[1][c] = (char)(h < 8 ? h - 1 : 7);
    lcdFrame[0][c] = h > 8 ? (char)(h - 9) : ' ';
  }
}

static void blinkPrompt(uint32_t now) {
  if (now - blinkAt < BLINK_MS) return;
  blinkAt = now;
  blinkOn = !blinkOn;
  if (blinkOn) lcdtext::center(PROMPT, lcdFrame[1], LCD_COLS);
  else memset(lcdFrame[1], ' ', LCD_COLS);
}

static void enterScreen(Screen s, uint32_t now) {
  screen = s;
  const bool track = s == SCR_TRACK;
  // Both lines are always reset so a stale scroll slot is released.
  mq[0].set(track ? artistTxt : s == SCR_IDLE ? nameTxt : "", track ? TRACK_CENTERED : true, now);
  mq[1].set(track ? titleTxt : "", TRACK_CENTERED, now);
  if (s == SCR_IDLE) {
    blinkOn = true;
    blinkAt = now;
    lcdtext::center(PROMPT, lcdFrame[1], LCD_COLS);
  } else if (s == SCR_WAIT) {
    waveAt = now;
  }
}

static void drawUi(uint32_t now, bool conn) {
  // Text only while playing and metadata exists; animation otherwise.
  const Screen want = !conn ? SCR_IDLE : (playing && hasMeta) ? SCR_TRACK : SCR_WAIT;
  if (want != screen) enterScreen(want, now);

  if (screen == SCR_WAIT) {
    drawWave(now);
  } else {
    mq[0].update(now, lcdFrame[0]);
    if (screen == SCR_TRACK) mq[1].update(now, lcdFrame[1]);
    else blinkPrompt(now);
  }
  flush();
}

// Write data to I2S
void read_data_stream(const uint8_t *data, uint32_t length) {
  i2s.write(data, length);
}

void setup() {
  //Serial.begin(115200);
  //AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // LCD first, so the idle screen shows while Bluetooth starts
  Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL, LCD_I2C_FREQ);
  lcdOk = lcd.begin(LCD_COLS, LCD_ROWS) == 0;
  if (lcdOk) loadGlyphs();
  memset(lcdFrame, ' ', sizeof lcdFrame);
  memset(lcdShown, ' ', sizeof lcdShown);
  lcdtext::sanitize(BT_DEVICE_NAME, nameTxt, sizeof nameTxt);
  drawUi(millis(), false);

  xTaskCreate(cmdLoop, "avrcCmd", 4096, nullptr, 2, &cmdTask);

  // Encoder switch only (rotation unused). Active low, the module has its own
  // pull-up. Fires on press, so nothing waits for a double-click timeout.
  btn.setup(PIN_ENCODER_SW, INPUT, true);
  btn.setDebounceMs(20);
  btn.attachPress(onPress);

  a2dp_sink.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
  a2dp_sink.set_avrc_metadata_callback(onMetadata);

  // register callback
  a2dp_sink.set_stream_reader(read_data_stream, false);

  // Start Bluetooth Audio Receiver
  a2dp_sink.set_auto_reconnect(false);
  a2dp_sink.start(BT_DEVICE_NAME);

  // setup output
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = CUSTOM_PIN_I2S_BCK;
  cfg.pin_data = CUSTOM_PIN_I2S_DATA_OUT;
  cfg.pin_data_rx = CUSTOM_PIN_I2S_DATA_IN;
  cfg.pin_mck = CUSTOM_PIN_I2S_MCK;
  cfg.pin_ws = CUSTOM_PIN_I2S_WS;
  cfg.sample_rate = a2dp_sink.sample_rate();
  cfg.channels = a2dp_sink.channels();
  cfg.i2s_format = I2S_STD_FORMAT;
  cfg.bits_per_sample = DEFAULT_BITS_PER_SAMPLE;
  cfg.buffer_count = I2S_BUFFER_COUNT;
  cfg.buffer_size = I2S_BUFFER_SIZE;
  i2s.begin(cfg);
}

void loop() {
  btn.tick();  // polled every pass, so loop() must never block

  const uint32_t now = millis();
  const bool conn = a2dp_sink.is_connected();
  static bool wasConn;
  if (wasConn && !conn) clearMeta();
  wasConn = conn;

  syncPlayState(conn);
  syncMeta(now);
  drawUi(now, conn);
  delay(1);  // yield the CPU; still ~1 kHz button polling
}
