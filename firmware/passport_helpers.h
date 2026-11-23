#pragma once
// 胸牌固件用到的小工具：中文截断、天气摘要、音量换算、Wi-Fi 省电、松手提示音、播放任务优先级、深度睡眠、缺字替换。
#include <cmath>
#include <cstdint>
#include <string>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/tcpip.h"
#include "passport_sleep.h"
#include "passport_text.h"
#include "lvgl.h"
#include "esphome/components/speaker/speaker.h"

// 按 UTF-8 字符边界截断，避免把中文字符切成半个导致乱码。
inline std::string clip_utf8(const std::string &s, size_t max_chars) {
  size_t chars = 0, i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (chars == max_chars) return s.substr(0, i) + "…";
    i += len;
    chars++;
  }
  return s;
}

// 把集成的天气摘要压成待机一行：「当前多云，气温13度，…；明天有雨，…」→「13° 多云 ・ 明天有雨」
inline std::string weather_brief_of(const std::string &digest) {
  auto field = [&](const std::string &seg, const char *prefix) -> std::string {
    if (seg.rfind(prefix, 0) != 0) return "";
    std::string rest = seg.substr(std::string(prefix).size());
    return rest.substr(0, rest.find("，"));
  };
  std::string now_seg, tomorrow;
  size_t pos = 0;
  while (pos <= digest.size()) {
    size_t end = digest.find("；", pos);
    std::string seg = digest.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (seg.size() >= 3 && seg.compare(seg.size() - 3, 3, "。") == 0) seg.resize(seg.size() - 3);
    if (seg.rfind("当前", 0) == 0) now_seg = seg;
    if (tomorrow.empty()) tomorrow = field(seg, "明天");
    if (end == std::string::npos) break;
    pos = end + std::string("；").size();
  }
  if (now_seg.empty()) return "";
  std::string out;
  size_t t = now_seg.find("气温");
  if (t != std::string::npos) {
    size_t s = t + std::string("气温").size();
    size_t e = now_seg.find("度", s);
    if (e != std::string::npos) out = now_seg.substr(s, e - s) + "° ";
  }
  out += field(now_seg, "当前");
  if (!tomorrow.empty()) out += " ・ 明天" + tomorrow;
  return out;
}

// 音量百分比 → ES8311 DAC 寄存器比例（ESPHome 按 0~255 线性写 REG32，0xBF=0dB，0xFF=+32dB）。
// 按官方 BSP（esp_codec_dev，PA 5V / DAC 3.3V）把范围限制在约 -49.5dB ~ -3.6dB，避免满音量削波。
inline float spk_level(float v) {
  if (v <= 0.0f) return 0.0f;
  if (v > 1.0f) v = 1.0f;
  return (92.0f + v * 92.0f) / 255.0f;
}

// 动态切换 Wi-Fi 调制解调器省电：息屏时开启 DTIM 睡眠，亮屏时切回全速。
inline void set_wifi_ps(bool save) {
  esp_wifi_set_ps(save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

// 说完松手/静音结束时的 42ms 双音上扬微提示音（784Hz -> 1046Hz，16kHz 单声道 s16le）。
// 仅在麦克风停止后播放，不与麦克风争抢共用 I2S。
inline void play_sent_chime(esphome::speaker::Speaker *spk) {
  if (spk == nullptr) return;
  static int16_t pcm[672];
  static bool inited = false;
  if (!inited) {
    constexpr int N = 672;  // 42ms @ 16kHz
    for (int i = 0; i < N; i++) {
      float freq = (i < N / 2) ? 784.0f : 1046.5f;
      float env = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * i / (N - 1)));
      pcm[i] = static_cast<int16_t>(env * 3400.0f * std::sin(2.0f * 3.14159265f * freq * i / 16000.0f));
    }
    inited = true;
  }
  spk->play(reinterpret_cast<const uint8_t *>(pcm), sizeof(pcm));
}

// 播放期间主循环持有 I2S 总线锁，一旦被 Wi-Fi 任务（23）优先级继承，要等放掉全部互斥锁才降回，
// 于是整段播放都以 23 运行，压住 ESPHome 默认 19 的 speaker_task；界面刷新或 API 处理超过 50ms
// 就会饿死 DMA，日志出现 "Event without matching write record"，声音卡顿。
// 每次播放都会新建 speaker_task，且刚建好时主循环往往正忙着刷界面，所以由独立的高优先级任务
// 每 5ms 检查一次，把它提到与 mic_task 相同的 23（DMA 预填了 50ms 静音，来得及）。
inline void start_speaker_prio_guard() {
  xTaskCreate(
      [](void *) {
        constexpr UBaseType_t prio = 23;
        for (;;) {
          TaskHandle_t h = xTaskGetHandle("speaker_task");
          if (h != nullptr && uxTaskPriorityGet(h) < prio) vTaskPrioritySet(h, prio);
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      },
      "spk_prio", 2048, nullptr, 24, nullptr);
}

// 电脑主机每 1ms 发一个 USB SOF 帧，充电头不发；ESP-IDF 的 tick 钩子据此维护连接状态
inline bool usb_host_present() { return usb_serial_jtag_is_connected(); }

inline WakeCause current_wake_cause() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return WakeCause::POWER_ON;
    case ESP_SLEEP_WAKEUP_GPIO:
      return WakeCause::KEY;
    case ESP_SLEEP_WAKEUP_TIMER:
      return WakeCause::TIMER;
    default:
      return WakeCause::OTHER;
  }
}

// 引脚与 hardware.yaml / 官方 bsp_pins.h 一致
constexpr gpio_num_t PIN_LCD_SCLK = GPIO_NUM_8, PIN_LCD_MOSI = GPIO_NUM_9, PIN_LCD_CS = GPIO_NUM_1,
                     PIN_LCD_DC = GPIO_NUM_20, PIN_LCD_BL = GPIO_NUM_21;

// 睡前让 ST7789 关显示并进入睡眠（0x28、0x10）。此时已不再刷屏，直接用 GPIO 按 SPI mode 0 发命令。
inline void lcd_sleep_bitbang() {
  const gpio_num_t pins[] = {PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_CS, PIN_LCD_DC};
  for (auto p : pins) {
    gpio_reset_pin(p);
    gpio_set_direction(p, GPIO_MODE_OUTPUT);
  }
  gpio_set_level(PIN_LCD_SCLK, 0);
  gpio_set_level(PIN_LCD_CS, 1);
  auto cmd = [&](uint8_t c) {
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_CS, 0);
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_LCD_MOSI, (c >> i) & 1);
      esp_rom_delay_us(1);
      gpio_set_level(PIN_LCD_SCLK, 1);
      esp_rom_delay_us(1);
      gpio_set_level(PIN_LCD_SCLK, 0);
    }
    gpio_set_level(PIN_LCD_CS, 1);
  };
  cmd(0x28);
  esp_rom_delay_us(20000);
  cmd(0x10);
  esp_rom_delay_us(120000);
}

// 背光脚睡眠期间不保持会随复位默认电平漂移，可能把背光点亮；拉低并保持到下次开机
inline void prepare_deep_sleep_hw() {
  lcd_sleep_bitbang();
  gpio_reset_pin(PIN_LCD_BL);
  gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LCD_BL, 0);
  gpio_hold_en(PIN_LCD_BL);
  gpio_deep_sleep_hold_en();
}

// 开机时解除背光脚保持，否则 LEDC 调光不生效、屏幕一直黑
inline void release_deep_sleep_hold() {
  gpio_hold_dis(PIN_LCD_BL);
  gpio_deep_sleep_hold_dis();
}

// 唤醒后用 DHCP INIT-REBOOT 申请上次的 IP。Mesh 节点常丢掉刚关联后的第一个包，lwIP 要等 1 秒才重发；
// 关联后 200ms、500ms 若仍在等确认就提前重发（只在 REBOOTING 状态下，不影响完整 DHCP 流程）。
inline void dhcp_resend_if_rebooting(void *) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  auto *nif = sta ? static_cast<struct netif *>(esp_netif_get_netif_impl(sta)) : nullptr;
  struct dhcp *d = nif ? netif_dhcp_data(nif) : nullptr;
  if (d != nullptr && d->state == DHCP_STATE_REBOOTING) dhcp_network_changed_link_up(nif);
}

inline void start_dhcp_fast_retry() {
  static esp_timer_handle_t timers[2];
  for (auto &t : timers) {
    esp_timer_create_args_t args{};
    args.callback = [](void *) { tcpip_callback(dhcp_resend_if_rebooting, nullptr); };
    args.name = "dhcp_retry";
    esp_timer_create(&args, &t);
  }
  esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
      [](void *, esp_event_base_t, int32_t, void *) {
        esp_timer_stop(timers[0]);
        esp_timer_stop(timers[1]);
        esp_timer_start_once(timers[0], 200 * 1000);
        esp_timer_start_once(timers[1], 500 * 1000);
      },
      nullptr);
}

// 按标签实际使用的字体去掉或替换字库里没有的字，避免屏幕上出现方框
inline std::string fit_label(lv_obj_t *label, const std::string &s) {
  const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  lv_font_glyph_dsc_t dsc;
  return sanitize_for_font(s, [&](uint32_t cp) { return cp < 0x20 || lv_font_get_glyph_dsc(font, &dsc, cp, 0); });
}

// 回答卡片：小框（聆听/思考，下方还有提示）固定两行、超出用省略号；大框（回答）自动换行铺满，
// 放不下时停 2.5 秒后慢慢向下滚到底，停留后回到开头，循环。卡片会裁掉超出部分。
static int32_t g_reply_visible_h = 50;

inline void reply_scroll_cb(void *obj, int32_t v) { lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), v, 0); }

inline void reply_autoscroll(lv_obj_t *lbl) {
  lv_anim_delete(lbl, reply_scroll_cb);
  lv_obj_set_style_translate_y(lbl, 0, 0);
  if (g_reply_visible_h <= 50) return;
  lv_obj_update_layout(lbl);
  const int32_t over = lv_obj_get_height(lbl) - g_reply_visible_h;
  if (over <= 0) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, lbl);
  lv_anim_set_exec_cb(&a, reply_scroll_cb);
  lv_anim_set_values(&a, 0, -over);
  lv_anim_set_duration(&a, over * 45);
  lv_anim_set_delay(&a, 2500);
  lv_anim_set_reverse_delay(&a, 3000);
  lv_anim_set_reverse_duration(&a, over * 8);
  lv_anim_set_repeat_delay(&a, 2500);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

inline void reply_set_visible(lv_obj_t *lbl, int32_t h) {
  g_reply_visible_h = h;
  if (h <= 50) {
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(lbl, h);
  } else {
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
  }
  reply_autoscroll(lbl);
}

// 文字变化导致高度变化时重新计算滚动
inline void reply_autoscroll_init(lv_obj_t *lbl) {
  lv_obj_add_event_cb(
      lbl, [](lv_event_t *e) { reply_autoscroll(static_cast<lv_obj_t *>(lv_event_get_target(e))); },
      LV_EVENT_SIZE_CHANGED, nullptr);
}
