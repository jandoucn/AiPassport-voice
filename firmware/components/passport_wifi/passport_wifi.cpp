#include "passport_wifi.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/captive_portal/captive_portal.h"

#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "portal_page.h"

namespace esphome::passport_wifi {
namespace {

static const char *const TAG = "passport_wifi";
constexpr char kNvsNamespace[] = "pp_wifi";
constexpr char kLegacyNvsNamespace[] = "nr_wifi";
constexpr char kCredentialsBlobKey[] = "credentials_v1";
constexpr uint8_t kCredentialsBlobVersion = 1;
constexpr uint32_t kConnectTimeoutMs = 24000;
constexpr std::size_t kMaxScan = 20;
wifi_ap_record_t s_scan_records[kMaxScan];

struct Credential {
  char ssid[33];
  char password[65];
};

struct StoredCredentials {
  uint8_t version;
  uint8_t count;
  Credential entries[kMaxNetworks];
};
static_assert(sizeof(StoredCredentials) == 2 + sizeof(Credential) * kMaxNetworks);

std::size_t load_credentials_blob(Credential *out, std::size_t capacity, bool *has_saved_state) {
  nvs_handle_t handle;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    *has_saved_state = false;
    return 0;
  }
  std::size_t length = 0;
  esp_err_t err = nvs_get_blob(handle, kCredentialsBlobKey, nullptr, &length);
  *has_saved_state = err != ESP_ERR_NVS_NOT_FOUND;
  if (err != ESP_OK || length != sizeof(StoredCredentials)) {
    nvs_close(handle);
    return 0;
  }
  StoredCredentials stored{};
  err = nvs_get_blob(handle, kCredentialsBlobKey, &stored, &length);
  nvs_close(handle);
  if (err != ESP_OK || stored.version != kCredentialsBlobVersion || stored.count > kMaxNetworks) {
    std::memset(&stored, 0, sizeof(stored));
    return 0;
  }
  const auto count = std::min<std::size_t>(stored.count, capacity);
  std::memcpy(out, stored.entries, sizeof(Credential) * count);
  std::memset(&stored, 0, sizeof(stored));
  return count;
}

std::size_t load_legacy_credentials(const char *nvs_namespace, Credential *out, std::size_t capacity,
                                    bool *has_saved_state) {
  nvs_handle_t handle;
  if (nvs_open(nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
    *has_saved_state = false;
    return 0;
  }
  uint8_t count = 0;
  const esp_err_t count_err = nvs_get_u8(handle, "count", &count);
  *has_saved_state = count_err != ESP_ERR_NVS_NOT_FOUND;
  if (count_err != ESP_OK) {
    nvs_close(handle);
    return 0;
  }
  std::size_t loaded = 0;
  for (uint8_t i = 0; i < count && loaded < capacity; i++) {
    char key[12];
    std::size_t length = sizeof(out[loaded].ssid);
    std::snprintf(key, sizeof(key), "s%u", i);
    if (nvs_get_str(handle, key, out[loaded].ssid, &length) != ESP_OK || !out[loaded].ssid[0])
      continue;
    length = sizeof(out[loaded].password);
    std::snprintf(key, sizeof(key), "p%u", i);
    if (nvs_get_str(handle, key, out[loaded].password, &length) != ESP_OK)
      out[loaded].password[0] = '\0';
    loaded++;
  }
  nvs_close(handle);
  return loaded;
}

std::size_t load_credentials(Credential *out, std::size_t capacity) {
  bool has_saved_state = false;
  auto count = load_credentials_blob(out, capacity, &has_saved_state);
  if (has_saved_state)
    return count;
  count = load_legacy_credentials(kNvsNamespace, out, capacity, &has_saved_state);
  if (has_saved_state)
    return count;
  return load_legacy_credentials(kLegacyNvsNamespace, out, capacity, &has_saved_state);
}

bool store_credentials(const Credential *credentials, std::size_t count) {
  nvs_handle_t handle;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  StoredCredentials stored{};
  stored.version = kCredentialsBlobVersion;
  stored.count = static_cast<uint8_t>(std::min<std::size_t>(count, kMaxNetworks));
  if (stored.count > 0)
    std::memcpy(stored.entries, credentials, sizeof(Credential) * stored.count);
  esp_err_t err = nvs_set_blob(handle, kCredentialsBlobKey, &stored, sizeof(stored));
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  std::memset(&stored, 0, sizeof(stored));
  return err == ESP_OK;
}

void wipe(Credential *credentials, std::size_t count) { std::memset(credentials, 0, sizeof(Credential) * count); }

std::string json_escape(const char *input) {
  std::string output;
  while (*input) {
    const unsigned char c = static_cast<unsigned char>(*input++);
    if (c == '"' || c == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(c));
    } else if (c >= 0x20) {
      output.push_back(static_cast<char>(c));
    }
  }
  return output;
}

}  // namespace

PassportWifi::PassportWifi() = default;

void PassportWifi::setup() {
  lock_ = xSemaphoreCreateMutex();
  if (lock_ == nullptr) {
    mark_failed();
    return;
  }
  uint8_t mac[6]{};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  std::snprintf(ap_ssid_, sizeof(ap_ssid_), "Passport-%02X%02X", mac[4], mac[5]);
  // 开机兜底热点与配网热点同名、不设密码，二维码始终有效
  wifi::WiFiAP ap = wifi::global_wifi_component->get_ap();
  ap.set_ssid(ap_ssid_);
  ap.set_password("");
  wifi::global_wifi_component->set_ap(ap);
  // 在 WiFi 组件启动前合并已保存的网络，让它直接扫描连接
  rebuild_wifi_networks_();
  web_server_base::global_web_server_base->add_handler_without_auth(this);
}

void PassportWifi::dump_config() {
  ESP_LOGCONFIG(TAG, "Passport Wi-Fi setup:");
  ESP_LOGCONFIG(TAG, "  Saved networks: %u/%u", static_cast<unsigned>(saved_count()),
                static_cast<unsigned>(kMaxNetworks));
  ESP_LOGCONFIG(TAG, "  Setup AP: %s", ap_ssid_);
}

void PassportWifi::loop() {
  const PendingAction action = pending_action_.exchange(PendingAction::None);
  if (action == PendingAction::Connect) {
    if (xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE) {
      std::memcpy(preferred_, pending_ssid_, sizeof(preferred_));
      std::memcpy(trial_password_, pending_password_, sizeof(trial_password_));
      std::memset(pending_password_, 0, sizeof(pending_password_));
      xSemaphoreGive(lock_);
    }
    rebuild_wifi_networks_();
    if (portal_active())
      set_portal_step_(PortalStep::Connecting);
    start_trial_();
  } else if (action == PendingAction::Rebuild) {
    rebuild_wifi_networks_();
  }

  const PortalStep step = portal_step_.load();
  if (step == PortalStep::Waiting) {
    wifi_sta_list_t stations{};
    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0)
      set_portal_step_(PortalStep::PhoneJoined);
  }
  if (!preferred_[0])
    return;
  char connected[33]{};
  const bool on_preferred = connected_ssid_(connected, sizeof(connected)) && std::strcmp(connected, preferred_) == 0;
  // 试连前可能已连在同名网络上（旧密码），先等这条旧连接断开，避免把错误密码当成成功
  if (!on_preferred)
    trial_dropped_ = true;
  if (on_preferred && trial_dropped_) {
    const bool saved = save_network_(preferred_, trial_password_);
    if (!saved)
      ESP_LOGE(TAG, "Joined the new network but could not save it");
    end_trial_(!saved);
    if (step == PortalStep::Connecting)
      set_portal_step_(saved ? PortalStep::Success : PortalStep::Failed);
  } else if (millis() - connecting_since_ >= kConnectTimeoutMs) {
    ESP_LOGW(TAG, "Could not join the new network, keeping saved networks");
    end_trial_(true);
    if (step == PortalStep::Connecting)
      set_portal_step_(PortalStep::Failed);
    wifi::global_wifi_component->retry_connect();
  }
}

std::size_t PassportWifi::saved_count() {
  char ssids[kMaxNetworks][33]{};
  return saved_networks_(ssids, kMaxNetworks);
}

std::size_t PassportWifi::saved_networks_(char (*ssids)[33], std::size_t max) {
  Credential credentials[kMaxNetworks]{};
  if (lock_ != nullptr)
    xSemaphoreTake(lock_, portMAX_DELAY);
  const auto stored = load_credentials(credentials, kMaxNetworks);
  if (lock_ != nullptr)
    xSemaphoreGive(lock_);
  std::size_t copied = 0;
  for (const auto &ap : builtin_) {
    if (copied < max)
      std::snprintf(ssids[copied++], 33, "%.32s", ap.get_ssid().c_str());
  }
  for (std::size_t i = 0; i < stored && copied < max; i++) {
    if (is_builtin_(credentials[i].ssid))
      continue;
    std::snprintf(ssids[copied++], 33, "%.32s", credentials[i].ssid);
  }
  wipe(credentials, kMaxNetworks);
  return copied;
}

bool PassportWifi::is_builtin_(const char *ssid) const {
  for (const auto &ap : builtin_) {
    if (ap.get_ssid() == ssid)
      return true;
  }
  return false;
}

bool PassportWifi::connected_ssid_(char *out, std::size_t capacity) {
  if (!wifi::global_wifi_component->is_connected())
    return false;
  char ssid[wifi::SSID_BUFFER_SIZE];
  const char *value = wifi::global_wifi_component->wifi_ssid_to(ssid);
  if (!value || !value[0])
    return false;
  std::snprintf(out, capacity, "%s", value);
  return true;
}

void PassportWifi::rebuild_wifi_networks_() {
  auto *wifi_component = wifi::global_wifi_component;
  Credential credentials[kMaxNetworks]{};
  xSemaphoreTake(lock_, portMAX_DELAY);
  const auto count = load_credentials(credentials, kMaxNetworks);
  xSemaphoreGive(lock_);
  // 试连期间：新网络排第一，同名的旧凭据（含内置）暂时移出，确保连上就说明新密码正确
  const bool trial = preferred_[0] != '\0';
  if (count > 0 || rebuilt_ || trial) {
    rebuilt_ = true;
    wifi_component->clear_sta();
    wifi_component->init_sta(builtin_.size() + count + (trial ? 1 : 0));
    if (trial) {
      wifi::WiFiAP network;
      network.set_ssid(preferred_);
      network.set_password(trial_password_);
      wifi_component->add_sta(network);
    }
    for (const auto &ap : builtin_) {
      if (trial && ap.get_ssid() == preferred_)
        continue;
      // 扫码保存的同名网络比 YAML 内置的更晚，以保存的为准（例如路由器改过密码）
      bool overridden = false;
      for (std::size_t i = 0; i < count && !overridden; i++)
        overridden = !is_builtin_(credentials[i].ssid) && ap.get_ssid() == credentials[i].ssid;
      if (overridden)
        continue;
      wifi_component->add_sta(ap);
    }
    for (std::size_t i = 0; i < count; i++) {
      if (is_builtin_(credentials[i].ssid) || (trial && std::strcmp(credentials[i].ssid, preferred_) == 0))
        continue;
      wifi::WiFiAP network;
      network.set_ssid(credentials[i].ssid);
      network.set_password(credentials[i].password);
      wifi_component->add_sta(network);
    }
  }
  wipe(credentials, kMaxNetworks);
}

bool PassportWifi::save_network_(const char *ssid, const char *password) {
  Credential old[kMaxNetworks]{};
  Credential next[kMaxNetworks]{};
  xSemaphoreTake(lock_, portMAX_DELAY);
  const auto count = load_credentials(old, kMaxNetworks);
  std::snprintf(next[0].ssid, sizeof(next[0].ssid), "%s", ssid);
  std::snprintf(next[0].password, sizeof(next[0].password), "%s", password);
  // 内置网络也占一个名额，总数不超过 kMaxNetworks
  const std::size_t limit = kMaxNetworks > builtin_.size() ? kMaxNetworks - builtin_.size() : 1;
  std::size_t written = 1;
  for (std::size_t i = 0; i < count && written < limit; i++) {
    if (std::strcmp(old[i].ssid, ssid) != 0)
      next[written++] = old[i];
  }
  const bool result = store_credentials(next, written);
  xSemaphoreGive(lock_);
  wipe(old, kMaxNetworks);
  wipe(next, kMaxNetworks);
  return result;
}

bool PassportWifi::remove_network_(const char *ssid) {
  Credential old[kMaxNetworks]{};
  Credential next[kMaxNetworks]{};
  xSemaphoreTake(lock_, portMAX_DELAY);
  const auto count = load_credentials(old, kMaxNetworks);
  std::size_t written = 0;
  for (std::size_t i = 0; i < count; i++) {
    if (std::strcmp(old[i].ssid, ssid) != 0)
      next[written++] = old[i];
  }
  const bool result = store_credentials(next, written);
  xSemaphoreGive(lock_);
  wipe(old, kMaxNetworks);
  wipe(next, kMaxNetworks);
  return result;
}

void PassportWifi::queue_connect_(const char *ssid, const char *password) {
  if (!ssid || !ssid[0])
    return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  std::snprintf(pending_ssid_, sizeof(pending_ssid_), "%s", ssid);
  std::snprintf(pending_password_, sizeof(pending_password_), "%s", password);
  xSemaphoreGive(lock_);
  pending_action_.store(PendingAction::Connect);
}

void PassportWifi::start_trial_() {
  if (!preferred_[0])
    return;
  wifi::WiFiAP network;
  network.set_ssid(preferred_);
  network.set_password(trial_password_);
  connecting_since_ = millis();
  trial_dropped_ = false;
  wifi::global_wifi_component->start_connecting(network);
}

void PassportWifi::end_trial_(bool restore) {
  preferred_[0] = '\0';
  std::memset(trial_password_, 0, sizeof(trial_password_));
  trial_dropped_ = false;
  // 连上了就不必重建列表（当前列表已经是新密码）；失败时恢复原有网络，避免一直拿错密码重连
  if (restore)
    rebuild_wifi_networks_();
}

void PassportWifi::start_portal() {
  if (portal_active())
    return;
  wifi_config_t access_point{};
  std::memcpy(access_point.ap.ssid, ap_ssid_, std::strlen(ap_ssid_));
  access_point.ap.ssid_len = std::strlen(ap_ssid_);
  access_point.ap.channel = 1;
  access_point.ap.authmode = WIFI_AUTH_OPEN;
  access_point.ap.max_connection = 2;
  restore_fallback_ap_ = captive_portal::global_captive_portal->is_active();
  if (!restore_fallback_ap_) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
      // 兜底热点已经在运行（开机没有可用网络），复用它，不改信道
      restore_fallback_ap_ = true;
    } else if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
      ESP_LOGE(TAG, "Could not enable the setup access point");
      return;
    }
  }
  if (!restore_fallback_ap_ && esp_wifi_set_config(WIFI_IF_AP, &access_point) != ESP_OK) {
    ESP_LOGE(TAG, "Could not configure the setup access point");
    esp_wifi_set_mode(WIFI_MODE_STA);
    return;
  }
  ESP_LOGI(TAG, "Setup portal open on %s", ap_ssid_);
  set_portal_step_(PortalStep::Waiting);
  if (!captive_portal::global_captive_portal->is_active())
    captive_portal::global_captive_portal->start();
}

void PassportWifi::stop_portal() {
  if (!portal_active())
    return;
  if (!restore_fallback_ap_) {
    if (captive_portal::global_captive_portal->is_active())
      captive_portal::global_captive_portal->end();
    esp_wifi_set_mode(WIFI_MODE_STA);
  }
  restore_fallback_ap_ = false;
  set_portal_step_(PortalStep::Off);
  ESP_LOGI(TAG, "Setup portal closed");
}

bool PassportWifi::canHandle(AsyncWebServerRequest *request) const {
  if (!portal_active())
    return false;
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(url_buffer);
  return url == "/" || url == "/scan" || url == "/saved" || url == "/connect" || url == "/forget" ||
         url == "/status";
}

void PassportWifi::handleRequest(AsyncWebServerRequest *request) {
  char url_buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(url_buffer);
  if (url == "/scan")
    serve_scan_(request);
  else if (url == "/saved")
    serve_saved_(request);
  else if (url == "/connect")
    serve_connect_(request);
  else if (url == "/forget")
    serve_forget_(request);
  else if (url == "/status")
    serve_status_(request);
  else
    request->send(200, "text/html; charset=utf-8", kPortalPage);
}

void PassportWifi::serve_scan_(AsyncWebServerRequest *request) {
  uint16_t count = kMaxScan;
  std::memset(s_scan_records, 0, sizeof(s_scan_records));
  wifi_scan_config_t config{};
  if (esp_wifi_scan_start(&config, true) != ESP_OK || esp_wifi_scan_get_ap_records(&count, s_scan_records) != ESP_OK)
    count = 0;
  std::sort(s_scan_records, s_scan_records + count,
            [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });
  std::string json = "[";
  for (uint16_t i = 0; i < count; i++) {
    const char *ssid = reinterpret_cast<const char *>(s_scan_records[i].ssid);
    if (!ssid[0])
      continue;
    bool duplicate = false;
    for (uint16_t previous = 0; previous < i; previous++)
      duplicate |= std::strcmp(ssid, reinterpret_cast<const char *>(s_scan_records[previous].ssid)) == 0;
    if (duplicate)
      continue;
    if (json.size() > 1)
      json += ',';
    json += "{\"s\":\"" + json_escape(ssid) + "\",\"r\":" + std::to_string(s_scan_records[i].rssi) +
            ",\"l\":" + (s_scan_records[i].authmode != WIFI_AUTH_OPEN ? "1" : "0") + "}";
  }
  json += ']';
  request->send(200, "application/json", json.c_str());
}

void PassportWifi::serve_saved_(AsyncWebServerRequest *request) {
  char ssids[kMaxNetworks][33]{};
  const auto count = saved_networks_(ssids, kMaxNetworks);
  std::string json = "[";
  for (std::size_t i = 0; i < count; i++) {
    if (i)
      json += ',';
    json += "{\"s\":\"" + json_escape(ssids[i]) + "\",\"b\":" + (is_builtin_(ssids[i]) ? "1" : "0") + "}";
  }
  json += ']';
  request->send(200, "application/json", json.c_str());
}

void PassportWifi::serve_connect_(AsyncWebServerRequest *request) {
  const std::string ssid = request->arg("s");
  const std::string password = request->arg("p");
  if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
    request->send(400, "text/plain", "bad fields");
    return;
  }
  // 先试连，连上后才保存，输错密码不会覆盖原有凭据
  queue_connect_(ssid.c_str(), password.c_str());
  request->send(200, "application/json", "{\"ok\":1}");
}

void PassportWifi::serve_forget_(AsyncWebServerRequest *request) {
  const std::string ssid = request->arg("s");
  if (is_builtin_(ssid.c_str())) {
    request->send(409, "text/plain", "built-in network");
    return;
  }
  if (ssid.empty() || !remove_network_(ssid.c_str())) {
    request->send(400, "text/plain", "bad fields");
    return;
  }
  pending_action_.store(PendingAction::Rebuild);
  request->send(200, "application/json", "{\"ok\":1}");
}

void PassportWifi::serve_status_(AsyncWebServerRequest *request) {
  const char *status = "idle";
  switch (portal_step_.load()) {
    case PortalStep::Failed:
      status = "failed";
      break;
    case PortalStep::Connecting:
      status = "connecting";
      break;
    case PortalStep::Success:
      status = "connected";
      break;
    default:
      break;
  }
  const std::string json = std::string("{\"st\":\"") + status + "\"}";
  request->send(200, "application/json", json.c_str());
}

}  // namespace esphome::passport_wifi
