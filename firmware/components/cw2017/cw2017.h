// CW2017 电量计：开机校验并写入电芯曲线，定时读取电量与电压。
// 寄存器与 500mAh 电芯曲线来自 FoloToy AI Passport 官方 BSP 的 bsp_battery.c（MIT）。
#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::cw2017 {

class CW2017 : public PollingComponent, public i2c::I2CDevice {
 public:
  void set_battery_level_sensor(sensor::Sensor *s) { battery_level_ = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s) { battery_voltage_ = s; }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool profile_matches_();
  void write_profile_();
  void wake_();

  sensor::Sensor *battery_level_{nullptr};
  sensor::Sensor *battery_voltage_{nullptr};
  uint8_t version_{0};
  bool profile_written_{false};
};

}  // namespace esphome::cw2017
