"""AiPassport-voice：为 FoloToy AI Passport 提供天气预报与本地快捷问答。"""

from __future__ import annotations

from dataclasses import dataclass

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant

from .const import NO_WEATHER

PLATFORMS = [Platform.SENSOR, Platform.CONVERSATION]


@dataclass
class PassportRuntime:
    """传感器与对话代理共享的天气摘要。"""

    weather_digest: str = NO_WEATHER


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    entry.runtime_data = PassportRuntime()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    entry.async_on_unload(entry.add_update_listener(_async_reload))
    return True


async def _async_reload(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
