# 网页一键刷机

适用于 **FoloToy AI Passport** 胸牌（ESP32-C3）。下方按钮用于 USB 首次安装，会覆盖原有固件。已配对的胸牌请使用 HA 的固件更新实体，或按[保留原配对的升级说明](https://github.com/jandoucn/AiPassport-voice#升级已配对的胸牌)操作。

## 准备

- 电脑上的 Chrome 或 Edge 浏览器（需要 Web Serial，Safari 和 Firefox 不支持）
- 能传数据的 USB-C 线
- 长按胸牌左侧电源键开机，再用 USB 连接电脑

## 刷机

<esp-web-install-button manifest="firmware/passport-voice.manifest.json"></esp-web-install-button>

点上面的按钮，选择胸牌对应的串口，按提示完成安装。

## 刷完之后

1. **连接 Wi-Fi**：胸牌屏幕会显示二维码。用手机扫码连上 `Passport-XXXX` 热点，在弹出的页面里选择 Wi-Fi 并输入密码。最多可以保存 4 个网络，比如家里的 Wi-Fi 和手机热点。
2. **接入 Home Assistant**：在 **设置 → 设备与服务** 中选择发现的胸牌。支持动态配对的 HA 会为设备设置 API 密钥；使用较早版本时，参见仓库的加密配置说明。
3. **配置语音助手**：在 **设置 → 语音助手** 中为胸牌选择语音识别、对话代理和语音合成。
4. **以后升级**：HA 的设备页会出现“固件”更新实体，有新版本时直接在 HA 里点更新。

之后想添加新的 Wi-Fi：按住胸牌的上键 5 秒，或在 HA 设备页按“添加 Wi-Fi”。

详细说明见 [GitHub 仓库]({{ site.github.repository_url }})。

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
