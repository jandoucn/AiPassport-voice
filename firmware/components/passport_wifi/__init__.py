import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NETWORKS, CONF_PASSWORD, CONF_SSID
from esphome.core import CORE

# 扫码配网与多 Wi-Fi 保存，移植自 FoloToy AI Passport 原厂固件的 nr_net（MIT，见 LICENSE-FoloToy）
DEPENDENCIES = ["wifi", "captive_portal"]
AUTO_LOAD = ["web_server_base"]

passport_wifi_ns = cg.esphome_ns.namespace("passport_wifi")
PassportWifi = passport_wifi_ns.class_("PassportWifi", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PassportWifi),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # wifi: 里写的网络作为内置网络，扫码保存的网络追加在后面
    for network in CORE.config.get("wifi", {}).get(CONF_NETWORKS, []):
        if ssid := network.get(CONF_SSID):
            cg.add(var.add_builtin(ssid, network.get(CONF_PASSWORD, "")))
