#include "cw2017.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::cw2017 {

static const char *const TAG = "cw2017";

static constexpr uint8_t REG_VERSION = 0x00;
static constexpr uint8_t REG_VCELL_H = 0x02;
static constexpr uint8_t REG_SOC_H = 0x04;
static constexpr uint8_t REG_CONFIG = 0x08;
static constexpr uint8_t REG_SOC_ALERT = 0x0B;
static constexpr uint8_t REG_PROFILE = 0x10;
static constexpr uint8_t PROFILE_SIZE = 80;
static constexpr uint8_t UPDATE_FLAG = 0x80;

// 万锂新能源 500mAh profile2 XIL40X 20261020
static const uint8_t PROFILE[PROFILE_SIZE] = {
    0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAF, 0xCA, 0xCA, 0xC0, 0xB4, 0xA8, 0xCB, 0xAA,
    0x9F, 0xCF, 0xB5, 0x8A, 0x70, 0x56, 0x4B, 0x40, 0x34, 0x2B, 0x22, 0x55, 0x41, 0xD2, 0x30, 0xD7,
    0xC9, 0xBE, 0xAE, 0xC8, 0xAE, 0x97, 0x9D, 0x98, 0x94, 0x9F, 0xB6, 0x89, 0x73, 0x65, 0x57, 0x49,
    0x3D, 0x50, 0x74, 0x87, 0xA3, 0x9B, 0x5E, 0x38, 0x00, 0x00, 0xAB, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7,
};

void CW2017::setup() {
  if (!this->read_byte(REG_VERSION, &this->version_)) {
    ESP_LOGE(TAG, "CW2017 not found");
    this->mark_failed();
    return;
  }
  // 同时检查更新标志和 80 字节曲线，避免误用旧电芯参数；一致时不重置电量计
  if (this->profile_matches_()) {
    uint8_t cfg = 0;
    if (this->read_byte(REG_CONFIG, &cfg) && cfg != 0x00)
      this->wake_();
  } else {
    this->write_profile_();
  }
}

bool CW2017::profile_matches_() {
  uint8_t alert = 0;
  if (!this->read_byte(REG_SOC_ALERT, &alert) || (alert & UPDATE_FLAG) == 0)
    return false;
  for (uint8_t i = 0; i < PROFILE_SIZE; i++) {
    uint8_t v = 0;
    if (!this->read_byte(REG_PROFILE + i, &v) || v != PROFILE[i])
      return false;
  }
  return true;
}

void CW2017::write_profile_() {
  this->write_byte(REG_CONFIG, 0x30);
  delay(20);
  this->write_byte(REG_CONFIG, 0xF0);
  delay(10);
  for (uint8_t i = 0; i < PROFILE_SIZE; i++)
    this->write_byte(REG_PROFILE + i, PROFILE[i]);
  uint8_t alert = 0;
  if (this->read_byte(REG_SOC_ALERT, &alert))
    this->write_byte(REG_SOC_ALERT, alert | UPDATE_FLAG);
  this->wake_();
  this->profile_written_ = true;
}

void CW2017::wake_() {
  this->write_byte(REG_CONFIG, 0x30);
  delay(20);
  this->write_byte(REG_CONFIG, 0x00);
  delay(10);
}

void CW2017::update() {
  uint8_t soc[2]{}, vcell[2]{};
  if (this->read_register(REG_SOC_H, soc, 2) != i2c::ERROR_OK ||
      this->read_register(REG_VCELL_H, vcell, 2) != i2c::ERROR_OK) {
    this->status_set_warning();
    return;
  }
  if (soc[0] > 100) {
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
  if (this->battery_level_ != nullptr)
    this->battery_level_->publish_state(soc[0]);
  if (this->battery_voltage_ != nullptr) {
    const uint16_t raw = ((static_cast<uint16_t>(vcell[0]) << 8) | vcell[1]) & 0x3FFF;
    this->battery_voltage_->publish_state(raw * 0.0003125f);
  }
}

void CW2017::dump_config() {
  ESP_LOGCONFIG(TAG, "CW2017 fuel gauge:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Version: 0x%02X, profile %s", this->version_,
                this->profile_written_ ? "written" : "matched");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Battery Level", this->battery_level_);
  LOG_SENSOR("  ", "Battery Voltage", this->battery_voltage_);
}

}  // namespace esphome::cw2017
