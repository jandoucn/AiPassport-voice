"""Kokoro (mlx-audio) 中文语音合成的 Wyoming 服务，供 Home Assistant 使用，支持流式合成。"""

import argparse
import asyncio
import logging
import re
import threading
from functools import partial

import numpy as np
from mlx_audio.tts.utils import load_model
from wyoming.audio import AudioChunk, AudioStart, AudioStop
from wyoming.event import Event
from wyoming.info import Attribution, Describe, Info, TtsProgram, TtsVoice
from wyoming.server import AsyncEventHandler, AsyncServer
from wyoming.tts import Synthesize, SynthesizeChunk, SynthesizeStart, SynthesizeStop, SynthesizeStopped

_LOG = logging.getLogger("kokoro_wyoming")
MODEL_ID = "mlx-community/Kokoro-82M-bf16"
VOICES = ["zf_xiaoxiao", "zf_xiaobei", "zf_xiaoni", "zf_xiaoyi", "zm_yunjian", "zm_yunxi", "zm_yunxia", "zm_yunyang"]
SENTENCE_END = re.compile(r"(.+?[。！？!?；;\n]+)", re.S)
SAMPLES_PER_CHUNK = 2048


class KokoroEngine:
    def __init__(self, default_voice: str, speed: float) -> None:
        self.model = load_model(MODEL_ID)
        self.rate = self.model.sample_rate
        self.default_voice = default_voice
        self.speed = speed
        self._lock = threading.Lock()
        self.pcm("你好。", default_voice)  # 预热

    def pcm(self, text: str, voice: str | None) -> bytes:
        voice = voice if voice in VOICES else self.default_voice
        with self._lock:
            parts = [np.array(r.audio) for r in self.model.generate(text=text, voice=voice, lang_code="z", speed=self.speed)]
        if not parts:
            return b""
        return (np.clip(np.concatenate(parts), -1, 1) * 32767).astype(np.int16).tobytes()


class Handler(AsyncEventHandler):
    def __init__(self, info: Info, engine: KokoroEngine, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.info, self.engine = info, engine
        self.streaming = False
        self.started = False
        self.buffer = ""
        self.voice: str | None = None

    async def _speak(self, text: str) -> None:
        text = text.strip()
        if not re.search(r"\w", text):
            return
        audio = await asyncio.to_thread(self.engine.pcm, text, self.voice)
        if not self.started:
            await self.write_event(AudioStart(rate=self.engine.rate, width=2, channels=1).event())
            self.started = True
        step = SAMPLES_PER_CHUNK * 2
        for i in range(0, len(audio), step):
            await self.write_event(AudioChunk(rate=self.engine.rate, width=2, channels=1, audio=audio[i : i + step]).event())

    async def _finish(self) -> None:
        if not self.started:
            await self.write_event(AudioStart(rate=self.engine.rate, width=2, channels=1).event())
        await self.write_event(AudioStop().event())
        self.started = False

    async def handle_event(self, event: Event) -> bool:
        if Describe.is_type(event.type):
            await self.write_event(self.info.event())
            return True

        if SynthesizeStart.is_type(event.type):
            start = SynthesizeStart.from_event(event)
            self.streaming, self.buffer = True, ""
            self.voice = start.voice.name if start.voice else None
            return True

        if SynthesizeChunk.is_type(event.type) and self.streaming:
            self.buffer += SynthesizeChunk.from_event(event).text
            while m := SENTENCE_END.match(self.buffer):
                self.buffer = self.buffer[m.end():]
                await self._speak(m.group(1))
            return True

        if SynthesizeStop.is_type(event.type) and self.streaming:
            await self._speak(self.buffer)
            self.buffer, self.streaming = "", False
            await self._finish()
            await self.write_event(SynthesizeStopped().event())
            return True

        if Synthesize.is_type(event.type):
            if self.streaming:
                return True  # 流式模式下 HA 会额外发一份完整文本，忽略
            synth = Synthesize.from_event(event)
            self.voice = synth.voice.name if synth.voice else None
            for sentence in SENTENCE_END.findall(synth.text + "\n"):
                await self._speak(sentence)
            await self._finish()
            return True

        return True


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uri", default="tcp://0.0.0.0:10210")
    parser.add_argument("--voice", default="zf_xiaoxiao", choices=VOICES)
    parser.add_argument("--speed", type=float, default=1.0)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    engine = KokoroEngine(args.voice, args.speed)
    attribution = Attribution(name="hexgrad / mlx-audio", url="https://huggingface.co/hexgrad/Kokoro-82M")
    info = Info(tts=[TtsProgram(
        name="kokoro", description="Kokoro 中文语音（Apple MLX）", attribution=attribution, installed=True,
        version="0.1", supports_synthesize_streaming=True,
        voices=[TtsVoice(name=v, description=v, attribution=attribution, installed=True, version=None, languages=["zh-CN", "zh"]) for v in VOICES],
    )])
    _LOG.info("Kokoro 已就绪：%s，默认音色 %s", args.uri, args.voice)
    await AsyncServer.from_uri(args.uri).run(partial(Handler, info, engine))


if __name__ == "__main__":
    asyncio.run(main())
