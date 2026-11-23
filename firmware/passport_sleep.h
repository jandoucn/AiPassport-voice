#pragma once
// 深度睡眠判断：纯逻辑，不依赖 ESP-IDF，可在电脑上单元测试（tests/firmware/test_sleep_policy.cpp）。
#include <cmath>

// 息屏（空闲 120 秒）后再等 5 分钟进入深度睡眠
constexpr int SLEEP_IDLE_SEC = 120 + 300;
// 电量计报到这个值就立刻睡，避免电池被放到 3.0V 被动断电
constexpr float SLEEP_LOW_BATT_PCT = 3.0f;

enum class SleepAction { STAY_AWAKE, SLEEP, SLEEP_LOW_BATTERY };

struct SleepInputs {
  int idle_sec;
  int scr_mode;  // 0 正常，1 微亮，2 息屏
  int va_phase;  // 0 空闲，1 聆听，2 思考/回答
  bool setup_open;
  bool usb_host;  // 插在电脑上：调试期间保持在线
  bool speaker_playing;
  bool allow_sleep;  // HA 中的「允许深度睡眠」开关
  float batt_pct;    // 电量计还没读到时为 NaN
  bool pending_ok;   // 按了 OK 正在等 HA 重连
  bool key_held;
};

// 低电量保护优先于 HA 开关，但仍要等对话、播放、配网结束，且插着电脑时不睡
inline SleepAction sleep_decision(const SleepInputs &in) {
  if (in.usb_host || in.va_phase != 0 || in.setup_open || in.speaker_playing || in.pending_ok || in.key_held)
    return SleepAction::STAY_AWAKE;
  if (!std::isnan(in.batt_pct) && in.batt_pct <= SLEEP_LOW_BATT_PCT)
    return SleepAction::SLEEP_LOW_BATTERY;
  if (in.allow_sleep && in.scr_mode == 2 && in.idle_sec >= SLEEP_IDLE_SEC)
    return SleepAction::SLEEP;
  return SleepAction::STAY_AWAKE;
}

enum class WakeCause { POWER_ON, KEY, TIMER, OTHER };

inline const char *wake_cause_text(WakeCause c) {
  switch (c) {
    case WakeCause::POWER_ON:
      return "上电/复位";
    case WakeCause::KEY:
      return "按键唤醒";
    case WakeCause::TIMER:
      return "定时唤醒";
    default:
      return "其他";
  }
}
