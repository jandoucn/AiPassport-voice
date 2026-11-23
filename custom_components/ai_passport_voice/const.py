"""常量。"""

from __future__ import annotations

DOMAIN = "ai_passport_voice"

CONF_FALLBACK_AGENT = "fallback_agent"
CONF_WEATHER_ENTITY = "weather_entity"

# 快捷问答未命中时转发的对话代理（可在集成选项中修改）
DEFAULT_FALLBACK_AGENT = "conversation.home_assistant"

# 天气摘要传感器的实体 ID，保持稳定便于在仪表盘和自动化中引用
ENTITY_ID_WEATHER = "sensor.shi_wai_tian_qi_yu_yu_bao"

NO_WEATHER = "暂无天气数据"
