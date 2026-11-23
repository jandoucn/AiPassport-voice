#pragma once
// 屏幕文字净化：纯逻辑，不依赖 ESP-IDF 和 LVGL，可在电脑上单元测试（tests/firmware/test_display_text.cpp）。
// 字库只编入 GB2312，大模型回复和语音识别里偶尔出现的 ·、—、emoji 等会显示成方框。
#include <cstdint>
#include <string>

inline void append_utf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// 字库缺字时的替代写法；返回 nullptr 表示没有替代，直接去掉
inline const char *display_fallback(uint32_t cp) {
  switch (cp) {
    case 0x00B7: case 0x2022: case 0x2027: case 0x2219: return "・";
    case 0x2014: return "―";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
    case 0x00B9: return "1";
    case 0x00B2: return "2";
    case 0x00B3: return "3";
    case 0x00BC: return "1/4";
    case 0x00BD: return "1/2";
    case 0x00BE: return "3/4";
    case 0x00A5: return "￥";
    case 0x20AC: return "欧元";
    case 0x00A3: return "英镑";
    case 0x2713: case 0x2714: case 0x2705: case 0x2611: return "√";
    case 0x2717: case 0x2718: case 0x274C: case 0x274E: return "×";
    case 0x26A0: case 0x2757: return "!";
    default: return nullptr;
  }
}

// 逐字检查字库：有就保留，缺字先找替代写法（替代写法也必须全部在字库里），否则去掉
template<typename HasGlyph> std::string sanitize_for_font(const std::string &s, HasGlyph has) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
      i++;
      continue;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    bool ok = true;
    for (size_t k = 1; k < len; k++) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc >> 6) != 0x2) ok = false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      i++;
      continue;
    }
    i += len;
    if (cp == '\n' || has(cp)) {
      append_utf8(out, cp);
      continue;
    }
    const char *alt = display_fallback(cp);
    if (alt == nullptr) continue;
    const std::string a(alt);
    bool all = true;
    for (size_t j = 0; j < a.size() && all;) {
      const auto ac = static_cast<unsigned char>(a[j]);
      const size_t al = ac < 0x80 ? 1 : (ac >> 5) == 0x6 ? 2 : (ac >> 4) == 0xE ? 3 : 4;
      uint32_t acp = al == 1 ? ac : al == 2 ? (ac & 0x1F) : al == 3 ? (ac & 0x0F) : (ac & 0x07);
      for (size_t k = 1; k < al; k++) acp = (acp << 6) | (static_cast<unsigned char>(a[j + k]) & 0x3F);
      all = has(acp);
      j += al;
    }
    if (all) out += a;
  }
  return out;
}
