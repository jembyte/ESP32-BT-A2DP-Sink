// LCD text helpers: UTF-8 folding, centering, yoradio-style marquee.
// No Arduino dependency, so it also builds and tests on a host.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace lcdtext {

// U+00C0..U+00FF and U+0100..U+017F folded to plain ASCII letters.
inline constexpr char kLatin1[] =
    "AAAAAAACEEEEIIII"   // C0..CF
    "DNOOOOOxOUUUUYTs"   // D0..DF
    "aaaaaaaceeeeiiii"   // E0..EF
    "dnooooo/ouuuuyty";  // F0..FF
inline constexpr char kLatinA[] =
    "AaAaAaCcCcCcCcDdDd"          // 0100..0111
    "EeEeEeEeEeGgGgGgGgHhHh"      // 0112..0127
    "IiIiIiIiIiIiJjKkk"           // 0128..0138
    "LlLlLlLlLlNnNnNnnNn"         // 0139..014B
    "OoOoOoOoRrRrRrSsSsSsSs"      // 014C..0161
    "TtTtTtUuUuUuUuUuUuWwYyY"     // 0162..0178
    "ZzZzZzs";                    // 0179..017F
static_assert(sizeof(kLatin1) == 65, "kLatin1 must cover 64 code points");
static_assert(sizeof(kLatinA) == 129, "kLatinA must cover 128 code points");

// UTF-8 -> LCD-safe ASCII. Folds accents, maps typographic punctuation, turns
// other symbols into a single '?', trims and collapses spaces.
// In-place safe (in == out): the output is never longer than the input.
inline void sanitize(const char* in, char* out, size_t cap) {
  if (!cap) return;
  size_t n = 0;
  bool unk = false;  // last output was a '?' for an unknown symbol
  auto put = [&](char c) {
    if (c == ' ' && (n == 0 || out[n - 1] == ' ')) return;
    if (n + 1 < cap) out[n++] = c;
    unk = false;
  };

  const uint8_t* p = (const uint8_t*)in;
  while (*p) {
    uint32_t cp = *p++;
    const int extra = cp >= 0xF8 ? 0 : cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    else if (cp >= 0x80) cp = 0xFFFD;  // stray continuation or invalid byte
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }  // truncated sequence
      cp = (cp << 6) | (*p++ & 0x3F);
    }

    if (cp < 0x20 || cp == 0x7F || cp == 0xA0 || cp == 0x3000 || cp == 0x202F ||
        cp == 0x205F || (cp >= 0x2002 && cp <= 0x200A)) {
      put(' ');
      continue;
    }
    if (cp < 0x7F) { put((char)cp); continue; }
    if ((cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || cp == 0xFEFF ||
        (cp >= 0xFE00 && cp <= 0xFE0F))
      continue;  // zero-width and variation selectors

    char one[2] = {0, 0};
    const char* rep = nullptr;
    switch (cp) {
      case 0xC6: rep = "AE"; break;
      case 0xE6: rep = "ae"; break;
      case 0x152: rep = "OE"; break;
      case 0x153: rep = "oe"; break;
      case 0xDF: rep = "ss"; break;
      case 0xA1: rep = "!"; break;
      case 0xBF: rep = "?"; break;
      case 0xB7: case 0x2022: rep = "*"; break;
      case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: rep = "'"; break;
      case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: rep = "\""; break;
      case 0x2026: rep = "..."; break;
      case 0x2212: rep = "-"; break;
      default:
        if (cp >= 0x2010 && cp <= 0x2015) rep = "-";
        else if (cp >= 0xC0 && cp <= 0xFF) one[0] = kLatin1[cp - 0xC0];
        else if (cp >= 0x100 && cp <= 0x17F) one[0] = kLatinA[cp - 0x100];
        if (one[0]) rep = one;
    }
    if (rep) {
      for (; *rep; rep++) put(*rep);
    } else if (!unk) {
      put('?');
      unk = true;
    }
  }
  while (n && out[n - 1] == ' ') n--;
  out[n] = 0;
}

// Centers s in out[cols] (space padded, not NUL-terminated); odd slack goes right.
inline void center(const char* s, char* out, uint8_t cols) {
  size_t n = strlen(s);
  if (n > cols) n = cols;
  memset(out, ' ', cols);
  memcpy(out + (cols - n) / 2, s, n);
}

// One LCD line. Text that fits is static; longer text scrolls like the
// yoradio LCD1602 title: hold, then jump kStepCols every kStepMs (leftwards),
// looping with " * ". Only one line scrolls at a time, as in yoradio.
template <uint8_t COLS, size_t CAP = 128>
class Marquee {
 public:
  ~Marquee() { if (owner_ == this) owner_ = nullptr; }  // never leave a dangling slot owner

  static constexpr uint32_t kHoldMs = 2000;  // pause before each scroll cycle
  static constexpr uint32_t kStepMs = 400;
  static constexpr uint8_t kStepCols = 2;
  static constexpr uint16_t kSepLen = 3;

  // Sets the text and restarts the scroll.
  void set(const char* s, bool centered, uint32_t now) {
    strlcpy(txt_, s, sizeof(txt_));
    len_ = (uint16_t)strlen(txt_);
    centered_ = centered;
    scroll_ = len_ > COLS;
    hidden_ = 0;
    stamp_ = now;
    dirty_ = true;
    if (owner_ == this) owner_ = nullptr;
  }

  // Writes out[COLS] (no NUL) and returns true when the view changed.
  bool update(uint32_t now, char* out) {
    if (scroll_ && (!owner_ || owner_ == this) &&
        now - stamp_ > (hidden_ ? kStepMs : kHoldMs)) {
      stamp_ = now;
      hidden_ += kStepCols;
      if (hidden_ > len_ + kSepLen) {  // cycle done: back to start, free the slot
        hidden_ = 0;
        owner_ = nullptr;
      } else {
        owner_ = this;
      }
      dirty_ = true;
    }
    if (!dirty_) return false;
    dirty_ = false;
    if (scroll_) {
      for (uint8_t i = 0; i < COLS; i++) out[i] = at(hidden_ + i);
    } else if (centered_) {
      center(txt_, out, COLS);
    } else {
      memset(out, ' ', COLS);
      memcpy(out, txt_, len_);
    }
    return true;
  }

 private:
  // Char i of "text + sep + text".
  char at(uint16_t i) const {
    if (i < len_) return txt_[i];
    i -= len_;
    if (i < kSepLen) return " * "[i];
    i -= kSepLen;
    return i < len_ ? txt_[i] : ' ';
  }

  static inline const Marquee* owner_ = nullptr;  // line currently scrolling
  char txt_[CAP] = {};
  uint16_t len_ = 0, hidden_ = 0;
  uint32_t stamp_ = 0;
  bool centered_ = false, scroll_ = false, dirty_ = false;
};

}  // namespace lcdtext
