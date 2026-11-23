// 扫码配网：手机连上胸牌的开放热点，在网页里添加 Wi-Fi。
// 最多保存 kMaxNetworks 个网络（存在 NVS 中，OTA 不会丢失），开机自动连接信号最强的一个。
// 移植自 FoloToy AI Passport 原厂固件的 nr_net（MIT，见 LICENSE-FoloToy）。
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esphome::passport_wifi {

enum class PortalStep : uint8_t {
  Off,
  Waiting,      // 热点已开启，等待手机连接
  PhoneJoined,  // 手机已连上热点
  Connecting,   // 已提交密码，正在连接
  Success,      // 连接成功，可以关闭热点
  Failed,       // 用提交的密码连接失败
};

constexpr std::size_t kMaxNetworks = 4;

class PassportWifi final : public Component, public AsyncWebHandler {
 public:
  PassportWifi();

  // YAML 里 wifi: 配置的网络，重建列表时保留
  void add_builtin(const char *ssid, const char *password) {
    wifi::WiFiAP ap;
    ap.set_ssid(ssid);
    ap.set_password(password);
    builtin_.push_back(ap);
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI + 2.0f; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  PortalStep portal_step() const { return portal_step_.load(); }
  bool portal_active() const { return portal_step_.load() != PortalStep::Off; }
  std::size_t saved_count();
  bool has_credentials() { return saved_count() > 0; }
  const char *ap_ssid() const { return ap_ssid_; }
  void start_portal();
  void stop_portal();

 protected:
  void set_portal_step_(PortalStep step) { portal_step_.store(step); }
  std::size_t saved_networks_(char (*ssids)[33], std::size_t max);
  bool connected_ssid_(char *out, std::size_t capacity);
  void rebuild_wifi_networks_();
  void start_trial_();
  void end_trial_(bool restore);
  void queue_connect_(const char *ssid, const char *password);
  bool save_network_(const char *ssid, const char *password);
  bool remove_network_(const char *ssid);
  void serve_scan_(AsyncWebServerRequest *request);
  void serve_saved_(AsyncWebServerRequest *request);
  void serve_connect_(AsyncWebServerRequest *request);
  void serve_forget_(AsyncWebServerRequest *request);
  void serve_status_(AsyncWebServerRequest *request);

  enum class PendingAction : uint8_t { None, Rebuild, Connect };

  std::atomic<PortalStep> portal_step_{PortalStep::Off};
  std::atomic<PendingAction> pending_action_{PendingAction::None};
  SemaphoreHandle_t lock_{nullptr};
  uint32_t connecting_since_{0};
  char pending_ssid_[33]{};
  char pending_password_[65]{};
  // 正在试连的新网络：连上后才写入 NVS，失败则丢弃，已保存的凭据保持不变
  char preferred_[33]{};
  char trial_password_[65]{};
  bool trial_dropped_{false};
  char ap_ssid_[20]{};
  bool restore_fallback_ap_{false};
  bool rebuilt_{false};
  std::vector<wifi::WiFiAP> builtin_;
  bool is_builtin_(const char *ssid) const;
};

}  // namespace esphome::passport_wifi
