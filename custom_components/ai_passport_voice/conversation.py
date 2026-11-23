"""对话代理：时间/日期/星期/天气本地毫秒级直答，其余转发给兜底对话代理。"""

from __future__ import annotations

import logging
import re
from dataclasses import replace
from datetime import datetime, timedelta
from typing import Literal

from homeassistant.components import conversation
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import intent
from homeassistant.util import dt as dt_util

from .const import CONF_FALLBACK_AGENT, DEFAULT_FALLBACK_AGENT, NO_WEATHER

_LOGGER = logging.getLogger(__name__)

AGENT_ENTITY_ID = "conversation.passport_voice"
_WEEKDAYS = ("一", "二", "三", "四", "五", "六", "日")
_PUNCT_RE = re.compile(r"[\s，。！？、；：,.!?;:'\"“”‘’（）()《》<>…—\-]+")


def _norm(text: str) -> str:
    return _PUNCT_RE.sub("", text).lower()


def _period(hour: int) -> str:
    if 5 <= hour < 9:
        return "早上"
    if 9 <= hour < 12:
        return "上午"
    if 12 <= hour < 14:
        return "中午"
    if 14 <= hour < 18:
        return "下午"
    if 18 <= hour < 24:
        return "晚上"
    return "凌晨"


def _date_str(now: datetime) -> str:
    return f"{now.year}年{now.month}月{now.day}日"


def _weather_segment(digest: str, text: str) -> str:
    """从摘要中取出指定时段的分句。"""
    segs = [s.strip().rstrip("。") for s in digest.split("；") if s.strip()]
    if not segs:
        return digest
    for label in ("后天", "明天"):
        if label in text:
            seg = next((s for s in segs if s.startswith(label)), None)
            return seg + "。" if seg else f"暂时获取不到{label}的天气预报。"
    return "；".join(s for s in segs if s.startswith(("当前", "今天"))) + "。"


def _requested_date(now: datetime, text: str) -> datetime:
    """Return the explicitly requested nearby date."""
    if "后天" in text:
        return now + timedelta(days=2)
    if "明天" in text:
        return now + timedelta(days=1)
    return now


class PassportVoiceAgent(conversation.ConversationEntity):
    """本地快捷问答 + 兜底转发的对话代理。"""

    _attr_name = "Passport Voice"
    _attr_has_entity_name = False

    def __init__(self, entry: ConfigEntry) -> None:
        self._entry = entry
        self._attr_unique_id = entry.entry_id

    @property
    def supported_languages(self) -> list[str] | Literal["*"]:
        return "*"

    async def async_internal_added_to_hass(self) -> None:
        self.entity_id = AGENT_ENTITY_ID
        await super().async_internal_added_to_hass()

    def _make_result(
        self, text: str, user_input: conversation.ConversationInput
    ) -> conversation.ConversationResult:
        response = intent.IntentResponse(language=user_input.language or "zh-CN")
        response.async_set_speech(text)
        return conversation.ConversationResult(
            response=response,
            conversation_id=user_input.conversation_id,
        )

    def _fast_answer(self, hass: HomeAssistant, text: str) -> str | None:
        t = _norm(text)
        now = dt_util.now()

        # 控制设备的句子必须交给 HA 的意图/对话代理处理。
        if any(
            k in t
            for k in (
                "打开",
                "开启",
                "关闭",
                "关掉",
                "调到",
                "调亮",
                "调暗",
                "调高",
                "调低",
                "设为",
                "设置",
                "切换",
                "增加",
                "减少",
            )
        ):
            return None

        # 带设备或房间名的状态查询也应由 HA 解析具体实体。
        if any(
            k in t
            for k in (
                "灯",
                "空调",
                "风扇",
                "窗帘",
                "开关",
                "传感器",
                "室内",
                "房间",
                "客厅",
                "卧室",
                "厨房",
                "卫生间",
                "书房",
                "阳台",
                "餐厅",
                "儿童房",
                "浴室",
                "玄关",
            )
        ):
            return None

        # 超出本地三日窗口的问题交给兜底代理，避免把相对日期答成今天。
        if any(k in t for k in ("昨天", "前天", "大后天", "下周", "下个月", "明年")):
            return None

        # 1) 时间
        if any(k in t for k in ("几点", "几时", "什么时间", "现在时间", "时间多少")):
            return f"现在是{_period(now.hour)}{now.hour}点{now.minute:02d}分。"

        # 2) 日期（几号 / 几月几号 / 几日）
        if any(k in t for k in ("几号", "几月几号", "几日", "日期")):
            requested = _requested_date(now, t)
            label = "后天" if "后天" in t else "明天" if "明天" in t else "今天"
            return f"{label}是{_date_str(requested)}，星期{_WEEKDAYS[requested.weekday()]}。"

        # 3) 星期几
        if (
            "星期几" in t
            or "周几" in t
            or "礼拜几" in t
            or ("星期" in t and "几" in t)
            or ("周" in t and "几" in t and "周末" not in t)
        ):
            requested = _requested_date(now, t)
            label = "后天" if "后天" in t else "明天" if "明天" in t else "今天"
            return f"{label}是{_date_str(requested)}，星期{_WEEKDAYS[requested.weekday()]}。"

        # 4) 天气（纯查询直答；带“穿/伞/建议”等决策词的交给大模型）
        weather_hit = any(
            k in t
            for k in (
                "天气",
                "气温",
                "温度",
                "多少度",
                "冷不冷",
                "热不冷",
                "冷不热",
                "下雨",
                "有雨",
                "降雨",
                "降水",
                "下雪",
                "有雪",
            )
        )
        advice_hit = any(
            k in t for k in ("穿", "带伞", "建议", "怎么办", "空气", "污染", "pm")
        )
        if weather_hit and not advice_hit:
            digest = self._entry.runtime_data.weather_digest
            if digest and digest != NO_WEATHER:
                return _weather_segment(digest, t)
            return "暂时获取不到天气数据。"

        return None

    async def _async_handle_message(
        self,
        user_input: conversation.ConversationInput,
        chat_log: conversation.ChatLog,
    ) -> conversation.ConversationResult:
        fast = self._fast_answer(self.hass, user_input.text)
        if fast is not None:
            return self._make_result(fast, user_input)

        agent_id = self._entry.options.get(CONF_FALLBACK_AGENT) or DEFAULT_FALLBACK_AGENT
        agent = conversation.async_get_agent(self.hass, agent_id)
        if agent is None or agent is self:
            agent = conversation.async_get_agent(self.hass, DEFAULT_FALLBACK_AGENT)
        if agent is None or agent is self:
            return self._make_result("抱歉，暂时没有可用的对话代理。", user_input)

        sub_input = replace(user_input, agent_id=getattr(agent, "entity_id", agent_id))
        try:
            if isinstance(agent, conversation.ConversationEntity):
                return await agent.internal_async_process(sub_input)
            return await agent.async_process(sub_input)
        except Exception as err:  # noqa: BLE001
            _LOGGER.warning("兜底对话代理 %s 调用失败: %s", agent_id, err)
            return self._make_result("抱歉，回答时出现了问题，请稍后再试。", user_input)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities,
) -> None:
    async_add_entities([PassportVoiceAgent(entry)])
