"""传感器：室外天气与预报，兼容每日、白天/夜间与逐小时三种预报。"""

from __future__ import annotations

import logging
from collections import Counter
from datetime import date, timedelta
from typing import Any

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import STATE_UNAVAILABLE, STATE_UNKNOWN
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import (
    async_call_later,
    async_track_state_change_event,
    async_track_time_interval,
)
from homeassistant.util import dt as dt_util

from .const import CONF_WEATHER_ENTITY, ENTITY_ID_WEATHER, NO_WEATHER

_LOGGER = logging.getLogger(__name__)

_COND_MAP = {
    "clear-night": "晴朗",
    "sunny": "晴",
    "partlycloudy": "多云",
    "cloudy": "阴",
    "rainy": "有雨",
    "pouring": "大雨",
    "lightning-rainy": "雷阵雨",
    "lightning": "雷暴",
    "snowy": "有雪",
    "snowy-rainy": "雨夹雪",
    "fog": "有雾",
    "windy": "大风",
    "hail": "冰雹",
    "windy-variant": "大风",
    "exceptional": "异常",
}

# WeatherEntityFeature：FORECAST_DAILY=1，FORECAST_TWICE_DAILY=2，FORECAST_HOURLY=4
_FORECAST_TYPES = (("daily", 1), ("twice_daily", 2), ("hourly", 4))
_DAY_LABELS = ("今天", "明天", "后天")


def _cond_cn(code: str | None) -> str:
    if not code:
        return "未知"
    return _COND_MAP.get(code, code)


def _num(v: Any) -> float | None:
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _fmt(v: float | None) -> str | None:
    return None if v is None else f"{v:.0f}"


def _to_celsius(v: float | None, unit: str | None) -> float | None:
    if v is None or unit not in ("°F", "F"):
        return v
    return (v - 32) * 5 / 9


def _to_mm(v: float | None, unit: str | None) -> float | None:
    if v is None:
        return None
    if unit == "in":
        return v * 25.4
    if unit == "cm":
        return v * 10
    return v


def _pick_weather_entity(
    hass: HomeAssistant, preferred: str | None = None
) -> str | None:
    """优先用集成选项里指定的实体，否则取第一个有实时温度的 weather 实体。"""

    def usable(state) -> bool:
        return (
            state is not None
            and state.state not in (STATE_UNKNOWN, STATE_UNAVAILABLE, "")
            and state.attributes.get("temperature") is not None
        )

    if preferred:
        return preferred if usable(hass.states.get(preferred)) else None
    candidates = sorted(
        s.entity_id for s in hass.states.async_all("weather") if usable(s)
    )
    return candidates[0] if candidates else None


def _extract_forecast(resp: Any, entity_id: str) -> list[dict]:
    """兼容 get_forecasts 的多种返回层级，取出 forecast 列表。"""
    if isinstance(resp, list):
        return resp
    if not isinstance(resp, dict):
        return []
    payload = resp.get("response", resp)
    if not isinstance(payload, dict):
        return []
    data = payload.get(entity_id)
    if data is None and "forecast" in payload:
        data = payload
    if data is None and payload:
        data = next(iter(payload.values()))
    if isinstance(data, dict):
        fc = data.get("forecast")
        return fc if isinstance(fc, list) else []
    if isinstance(data, list):
        return data
    return []


def _item_date(item: dict) -> date | None:
    raw = item.get("datetime")
    parsed = dt_util.parse_datetime(raw) if isinstance(raw, str) else raw
    if parsed is None:
        return None
    return dt_util.as_local(parsed).date()


def _summarize_days(
    forecast: list[dict], kind: str, today: date, temp_unit: str | None,
    precip_unit: str | None,
) -> dict[date, dict]:
    """把任意粒度的预报合并成按本地日期的 {hi, lo, cond, precip}。"""
    buckets: dict[date, list[dict]] = {}
    undated: list[dict] = []
    for item in forecast:
        d = _item_date(item)
        if d is None:
            undated.append(item)
        elif today <= d <= today + timedelta(days=2):
            buckets.setdefault(d, []).append(item)
    # 没有日期字段的每日预报按顺序对应今天、明天、后天
    if not buckets and kind == "daily":
        for i, item in enumerate(undated[:3]):
            buckets[today + timedelta(days=i)] = [item]

    days: dict[date, dict] = {}
    for d, items in buckets.items():
        highs, lows, precips = [], [], []
        for item in items:
            hi = _to_celsius(_num(item.get("temperature")), temp_unit)
            lo = _to_celsius(_num(item.get("templow")), temp_unit)
            if hi is not None:
                highs.append(hi)
            lows.append(lo if lo is not None else hi)
            p = _to_mm(_num(item.get("precipitation")), precip_unit)
            if p is not None:
                precips.append(p)
        lows = [v for v in lows if v is not None]
        if kind == "twice_daily":
            daytime = [i for i in items if i.get("is_daytime")] or items
            cond = daytime[0].get("condition")
        elif kind == "hourly":
            daytime = [
                i for i in items
                if (dt := dt_util.parse_datetime(str(i.get("datetime", ""))))
                and 6 <= dt_util.as_local(dt).hour < 19
            ] or items
            conds = [i.get("condition") for i in daytime if i.get("condition")]
            cond = Counter(conds).most_common(1)[0][0] if conds else None
        else:
            cond = items[0].get("condition")
        days[d] = {
            "hi": max(highs) if highs else None,
            "lo": min(lows) if lows else None,
            "cond": cond,
            "precip": sum(precips) if precips else 0.0,
        }
    return days


def _build_weather_digest(
    attrs: dict, condition: str, days: dict[date, dict], today: date
) -> str:
    temp_unit = attrs.get("temperature_unit")
    temp = _fmt(_to_celsius(_num(attrs.get("temperature")), temp_unit))
    hum = _fmt(_num(attrs.get("humidity")))
    head = f"当前{_cond_cn(condition)}"
    if temp is not None:
        head += f"，气温{temp}度"
    if hum is not None:
        head += f"，湿度{hum}%"
    parts = [head]
    for offset, label in enumerate(_DAY_LABELS):
        day = days.get(today + timedelta(days=offset))
        if day is None:
            continue
        seg = f"{label}{_cond_cn(day['cond'])}"
        lo, hi = _fmt(day["lo"]), _fmt(day["hi"])
        if lo is not None and hi is not None:
            seg += f"，{lo}到{hi}度" if lo != hi else f"，{hi}度"
        if day["precip"] >= 0.05:
            seg += f"，降水{day['precip']:.1f}".rstrip("0").rstrip(".") + "毫米"
        parts.append(seg)
    return "；".join(parts) + "。"


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities
) -> None:
    async_add_entities([WeatherDigestSensor(entry)])


class WeatherDigestSensor(SensorEntity):
    """室外天气 + 今明后三日预报摘要，供语音助手直答。"""

    _attr_name = "室外天气与预报"
    _attr_icon = "mdi:weather-partly-cloudy"
    _attr_unique_id = "passport_weather_digest"

    def __init__(self, entry: ConfigEntry) -> None:
        self._entry = entry
        self._digest = NO_WEATHER
        self._source: str | None = None
        self._forecast_type: str | None = None
        self._tracked: str | None = None
        self._untrack = None
        self._timer = None

    @property
    def native_value(self) -> str:
        return self._digest

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {"source_entity": self._source, "forecast_type": self._forecast_type}

    async def async_internal_added_to_hass(self) -> None:
        self.entity_id = ENTITY_ID_WEATHER
        await super().async_internal_added_to_hass()

    async def async_added_to_hass(self) -> None:
        await self._refresh()
        self.async_write_ha_state()
        self.async_on_remove(
            async_track_time_interval(
                self.hass, self._schedule_refresh, timedelta(minutes=30)
            )
        )
        if not self._entry.options.get(CONF_WEATHER_ENTITY):
            # 自动选择模式：有新的 weather 实体上线时重新挑选
            self.async_on_remove(
                self.hass.bus.async_listen("state_changed", self._on_any_state)
            )

    async def async_will_remove_from_hass(self) -> None:
        if self._untrack is not None:
            self._untrack()
        if self._timer is not None:
            self._timer()

    @callback
    def _on_any_state(self, event) -> None:
        if self._source is None and event.data.get("entity_id", "").startswith(
            "weather."
        ):
            self._schedule_refresh()

    @callback
    def _track(self, entity_id: str | None) -> None:
        if entity_id == self._tracked:
            return
        if self._untrack is not None:
            self._untrack()
            self._untrack = None
        self._tracked = entity_id
        if entity_id:
            self._untrack = async_track_state_change_event(
                self.hass, [entity_id], self._schedule_refresh
            )

    @callback
    def _schedule_refresh(self, _event=None) -> None:
        if self._timer is not None:
            self._timer()
        self._timer = async_call_later(
            self.hass, timedelta(seconds=5), self._refresh_and_write
        )

    async def _refresh_and_write(self, _now=None) -> None:
        self._timer = None
        await self._refresh()
        self.async_write_ha_state()

    async def _fetch_forecast(
        self, entity_id: str, features: int
    ) -> tuple[str | None, list[dict]]:
        for kind, flag in _FORECAST_TYPES:
            if features and not features & flag:
                continue
            try:
                resp = await self.hass.services.async_call(
                    "weather",
                    "get_forecasts",
                    {"type": kind},
                    target={"entity_id": entity_id},
                    blocking=True,
                    return_response=True,
                )
            except Exception as err:  # noqa: BLE001
                _LOGGER.debug("%s 不支持 %s 预报: %s", entity_id, kind, err)
                continue
            forecast = _extract_forecast(resp, entity_id)
            if forecast:
                return kind, forecast
        return None, []

    async def _refresh(self) -> None:
        hass = self.hass
        entity_id = _pick_weather_entity(
            hass, self._entry.options.get(CONF_WEATHER_ENTITY)
        )
        self._track(entity_id or self._entry.options.get(CONF_WEATHER_ENTITY))
        self._source = entity_id
        state = hass.states.get(entity_id) if entity_id else None
        if state is None:
            self._digest = NO_WEATHER
            self._forecast_type = None
        else:
            attrs = state.attributes
            kind, forecast = await self._fetch_forecast(
                entity_id, int(attrs.get("supported_features") or 0)
            )
            today = dt_util.now().date()
            days = _summarize_days(
                forecast, kind or "daily", today,
                attrs.get("temperature_unit"), attrs.get("precipitation_unit"),
            )
            self._forecast_type = kind
            self._digest = _build_weather_digest(attrs, state.state, days, today)
        self._entry.runtime_data.weather_digest = self._digest

    async def async_update(self) -> None:
        await self._refresh()
