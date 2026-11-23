# 第三方声明

本仓库自己的代码以 [MIT](LICENSE) 发布。以下内容来自第三方，按各自的许可证使用。

| 内容 | 来源 | 许可证 |
| --- | --- | --- |
| `firmware/components/voice_assistant/voice_assistant.cpp`、`voice_assistant.h` | 基于 [ESPHome 2026.10.2](https://github.com/esphome/esphome/tree/9a78022ded2285861f10de50168a0aa71e2a4322/esphome/components/voice_assistant) 修改（2026-11-21 至 2026-11-23）：松手发送音频结束标记、打断时丢弃旧回答、停止请求和结束标记失败重试、关闭自动追问 | GPLv3，见 [LICENSE](firmware/components/voice_assistant/LICENSE) |
| `firmware/components/voice_assistant/__init__.py` | [ESPHome 2026.10.2](https://github.com/esphome/esphome/blob/9a78022ded2285861f10de50168a0aa71e2a4322/esphome/components/voice_assistant/__init__.py)，未修改 | MIT，见同一文件 |
| `firmware/components/passport_wifi/` | 移植自 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 原厂固件的 `nr_net` 配网逻辑 | MIT，见 [FoloToy MIT 许可证](firmware/components/passport_wifi/LICENSE-FoloToy) |
| `firmware/components/cw2017/` 中的电芯曲线，`firmware/packages/hardware.yaml` 中的引脚定义 | [FoloToy AI Passport 官方 BSP](https://github.com/FoloToy/ai-passport/tree/488beda41309f052d81f08d0016d65a3fd85eb7e/components/bsp)（`bsp_battery.c`、`bsp_pins.h`） | MIT，见 [FoloToy MIT 许可证](firmware/components/passport_wifi/LICENSE-FoloToy) |
| `firmware/assets/fonts/NotoSansSC-Regular.otf` | [Noto Sans SC](https://github.com/notofonts/noto-cjk)；字体内嵌版权声明为 Copyright 2014-2021 Adobe，Noto 为 Google Inc. 商标 | SIL OFL 1.1，版权声明及许可证见 [OFL.txt](firmware/assets/fonts/OFL.txt) |

## 关于编译出的固件

固件由 ESPHome 编译，链接了 ESPHome 的 C++ 运行时（GPLv3）。分发的固件二进制按 GPLv3 提供。每个发布版本的对应源码由该版本的仓库 tag 与 [ESPHome 2026.10.2 源码](https://github.com/esphome/esphome/tree/9a78022ded2285861f10de50168a0aa71e2a4322) 共同组成；构建入口为 `firmware/passport-voice.factory.yaml`，依赖版本固定在发布工作流中。

## 商标

Home Assistant 是 Open Home Foundation 的商标，ESPHome 是 Open Home Foundation 的项目，FoloToy 与 AI Passport 归其各自所有者。本项目与上述组织没有隶属关系，文中提及仅用于说明兼容性。
