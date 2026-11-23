"""配置流：添加时可选天气实体和兜底对话代理，之后在选项中修改。"""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.config_entries import (
    ConfigEntry,
    ConfigFlow,
    OptionsFlow,
)
from homeassistant.core import callback
from homeassistant.data_entry_flow import FlowResult
from homeassistant.helpers import selector

from .const import (
    CONF_FALLBACK_AGENT,
    CONF_WEATHER_ENTITY,
    DEFAULT_FALLBACK_AGENT,
    DOMAIN,
)


def _schema(values: dict[str, Any]) -> vol.Schema:
    weather = values.get(CONF_WEATHER_ENTITY)
    return vol.Schema(
        {
            vol.Optional(
                CONF_WEATHER_ENTITY,
                description={"suggested_value": weather} if weather else None,
            ): selector.EntitySelector(
                selector.EntitySelectorConfig(domain="weather")
            ),
            vol.Required(
                CONF_FALLBACK_AGENT,
                default=values.get(CONF_FALLBACK_AGENT, DEFAULT_FALLBACK_AGENT),
            ): selector.ConversationAgentSelector(),
        }
    )


class PassportVoiceConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None) -> FlowResult:
        if self._async_current_entries():
            return self.async_abort(reason="single_instance_allowed")
        await self.async_set_unique_id(DOMAIN)
        self._abort_if_unique_id_configured()
        if user_input is not None:
            return self.async_create_entry(
                title="AI Passport 语音", data={}, options=user_input
            )
        return self.async_show_form(step_id="user", data_schema=_schema({}))

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return PassportVoiceOptionsFlow()


class PassportVoiceOptionsFlow(OptionsFlow):
    async def async_step_init(self, user_input=None) -> FlowResult:
        if user_input is not None:
            return self.async_create_entry(data=user_input)
        return self.async_show_form(
            step_id="init", data_schema=_schema(dict(self.config_entry.options))
        )
