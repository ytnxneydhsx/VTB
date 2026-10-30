#!/usr/bin/env python3
"""Run the fixed-shape RVC v2 pipeline with RKNNLite on an RK3588.

The neural HuBERT, RMVPE, and RVC generator graphs execute through RKNNLite.
Wave I/O, the RMVPE mel frontend/F0 decoder, NSF source generation, and SOLA
run in NumPy on the CPU.
"""

from __future__ import annotations

import argparse
import json
import time
import wave
from pathlib import Path

import numpy as np
from rknnlite.api import RKNNLite


CONTENT_RATE = 16_000
MODEL_RATE = 40_000
BLOCK_INPUT = 800
CONTENT_INPUT = 2_080
RMVPE_INPUT = 4_960
CONTENT_FRAMES = 13
OUTPUT_FRAMES = 8
OUTPUT_BLOCK = 2_000
CROSSFADE = 800
SOLA_SEARCH = 400
GENERATOR_OUTPUT = 3_200
RMVPE_FRAMES = 32
RMVPE_BINS = 360
F0_CACHE_SIZE = 1_024
F0_SHIFT = 5
NSF_UPSAMPLE = 400


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the fixed 40 kHz RVC v2 RKNN pipeline on RK3588."
    )
    parser.add_argument("--hubert", type=Path, required=True)
    parser.add_argument("--rmvpe", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--mel-basis", type=Path, required=True)
    parser.add_argument("--voice-params", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metrics", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261030)
    parser.add_argument("--max-blocks", type=int, default=0)
    return parser.parse_args()


def require_file(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise FileNotFoundError(resolved)
    return resolved


def read_pcm16_mono(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as audio_file:
        channels = audio_file.getnchannels()
        sample_width = audio_file.getsampwidth()
        sample_rate = audio_file.getframerate()
        frame_count = audio_file.getnframes()
        if channels != 1:
            raise ValueError(f"input must be mono, got {channels} channels")
        if sample_width != 2:
            raise ValueError(f"input must be PCM16, got {sample_width * 8} bits")
        if sample_rate != CONTENT_RATE:
            raise ValueError(
                f"input must be {CONTENT_RATE} Hz, got {sample_rate} Hz"
            )
        pcm = audio_file.readframes(frame_count)
    return np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0


def write_pcm16_mono(path: Path, audio: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    clipped = np.clip(audio, -1.0, 1.0)
    pcm = np.rint(clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as audio_file:
        audio_file.setnchannels(1)
        audio_file.setsampwidth(2)
        audio_file.setframerate(MODEL_RATE)
        audio_file.writeframes(pcm.tobytes())


class RknnSession:
    def __init__(self, model_path: Path, core_mask: int, label: str):
        self._label = label
        self._runtime = RKNNLite(verbose=False)
        self._released = False
        result = self._runtime.load_rknn(str(model_path))
        if result != 0:
            self._runtime.release()
            raise RuntimeError(f"{label}: load_rknn failed with code {result}")
        result = self._runtime.init_runtime(core_mask=core_mask)
        if result != 0:
            self._runtime.release()
            raise RuntimeError(f"{label}: init_runtime failed with code {result}")

    def infer(self, inputs: list[np.ndarray]) -> list[np.ndarray]:
        outputs = self._runtime.inference(inputs=inputs)
        if outputs is None:
            raise RuntimeError(f"{self._label}: inference returned no outputs")
        return outputs

    def release(self) -> None:
        if self._released:
            return
        self._runtime.release()
        self._released = True


def extract_mel(audio: np.ndarray, mel_basis: np.ndarray) -> np.ndarray:
    if audio.shape != (RMVPE_INPUT,):
        raise ValueError(f"unexpected RMVPE audio shape: {audio.shape}")
    padded = np.pad(audio, (512, 512), mode="reflect")
    frames = np.lib.stride_tricks.sliding_window_view(padded, 1024)[::160]
    if frames.shape != (RMVPE_FRAMES, 1024):
        raise ValueError(f"unexpected STFT frame shape: {frames.shape}")
    window = np.hanning(1025)[:-1].astype(np.float32)
    magnitude = np.abs(np.fft.rfft(frames * window, axis=1)).astype(np.float32)
    mel = mel_basis @ magnitude.T
    return np.log(np.maximum(mel, np.float32(1e-5))).astype(np.float32)[None]


def decode_rmvpe(salience: np.ndarray) -> np.ndarray:
    if salience.shape == (1, RMVPE_FRAMES, RMVPE_BINS):
        salience = salience[0]
    if salience.shape != (RMVPE_FRAMES, RMVPE_BINS):
        raise ValueError(f"unexpected RMVPE output shape: {salience.shape}")
    center = np.argmax(salience, axis=1)
    padded = np.pad(salience, ((0, 0), (4, 4)))
    cents = 20.0 * np.arange(RMVPE_BINS) + 1997.3794084376191
    cents = np.pad(cents, (4, 4))
    local_cents = np.empty(RMVPE_FRAMES, dtype=np.float64)
    for frame_index in range(RMVPE_FRAMES):
        start = int(center[frame_index])
        local_salience = padded[frame_index, start : start + 9]
        local_mapping = cents[start : start + 9]
        weight_sum = float(local_salience.sum())
        if weight_sum == 0.0:
            local_cents[frame_index] = 0.0
        else:
            local_cents[frame_index] = float(
                np.sum(local_salience * local_mapping) / weight_sum
            )
    local_cents[np.max(padded, axis=1) <= 0.03] = 0.0
    f0 = 10.0 * np.power(2.0, local_cents / 1200.0)
    f0[f0 == 10.0] = 0.0
    unvoiced = f0 == 0.0
    if np.any(~unvoiced):
        f0[unvoiced] = np.interp(
            np.flatnonzero(unvoiced),
            np.flatnonzero(~unvoiced),
            f0[~unvoiced],
        )
    return f0.astype(np.float32)


def coarse_pitch(f0: np.ndarray) -> np.ndarray:
    f0_mel_min = 1127.0 * np.log(1.0 + 50.0 / 700.0)
    f0_mel_max = 1127.0 * np.log(1.0 + 1100.0 / 700.0)
    f0_mel = 1127.0 * np.log(1.0 + f0 / 700.0)
    positive = f0_mel > 0.0
    f0_mel[positive] = (
        (f0_mel[positive] - f0_mel_min)
        * 254.0
        / (f0_mel_max - f0_mel_min)
        + 1.0
    )
    f0_mel[f0_mel <= 1.0] = 1.0
    f0_mel[f0_mel > 255.0] = 255.0
    return np.rint(f0_mel).astype(np.int64)


def update_pitch_cache(
    f0: np.ndarray,
    pitch_cache: np.ndarray,
    pitchf_cache: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    pitch = coarse_pitch(f0)
    pitch_cache[:-F0_SHIFT] = pitch_cache[F0_SHIFT:]
    pitchf_cache[:-F0_SHIFT] = pitchf_cache[F0_SHIFT:]
    pitch_cache[4 - pitch.shape[0] :] = pitch[3:-1]
    pitchf_cache[4 - f0.shape[0] :] = f0[3:-1]
    return (
        pitch_cache[-CONTENT_FRAMES:][None].copy(),
        pitchf_cache[-CONTENT_FRAMES:][None].copy(),
    )


def harmonic_source(
    pitchf: np.ndarray,
    linear_weight: float,
    linear_bias: float,
    rng: np.random.Generator,
) -> np.ndarray:
    f0 = pitchf[:, 5 : 5 + OUTPUT_FRAMES]
    sample_positions = np.arange(1, NSF_UPSAMPLE + 1, dtype=np.float32)
    radians = f0[..., None] / np.float32(MODEL_RATE) * sample_positions
    frame_phase = np.fmod(radians[..., -1:] + 0.5, 1.0) - 0.5
    accumulated = np.fmod(np.cumsum(frame_phase, axis=1), 1.0)
    previous = np.concatenate(
        (np.zeros_like(accumulated[:, :1]), accumulated[:, :-1]), axis=1
    )
    radians = (radians + previous).reshape(1, GENERATOR_OUTPUT, 1)
    sine = np.sin(2.0 * np.pi * radians).astype(np.float32) * 0.1
    voiced = (f0 > 0.0).astype(np.float32)[..., None]
    voiced = np.repeat(voiced, NSF_UPSAMPLE, axis=1)
    noise_amplitude = voiced * 0.003 + (1.0 - voiced) * (0.1 / 3.0)
    noise = rng.standard_normal(sine.shape).astype(np.float32) * noise_amplitude
    source = sine * voiced + noise
    merged = np.tanh(source * linear_weight + linear_bias).astype(np.float32)
    return np.transpose(merged, (0, 2, 1))


def sola_align(
    generated: np.ndarray,
    sola_buffer: np.ndarray,
    fade_in: np.ndarray,
) -> tuple[np.ndarray, int]:
    if generated.shape != (GENERATOR_OUTPUT,):
        raise ValueError(f"unexpected generator output shape: {generated.shape}")
    front = generated[: CROSSFADE + SOLA_SEARCH]
    numerator = np.correlate(front, sola_buffer, mode="valid")
    squares = np.concatenate(
        (np.zeros(1, dtype=np.float64), np.cumsum(front * front, dtype=np.float64))
    )
    energy = squares[CROSSFADE:] - squares[:-CROSSFADE]
    score = numerator / np.sqrt(energy + 1e-8)
    offset = int(np.argmax(score))
    aligned = generated[offset:].copy()
    aligned[:CROSSFADE] = (
        aligned[:CROSSFADE] * fade_in + sola_buffer * (1.0 - fade_in)
    )
    sola_buffer[:] = aligned[OUTPUT_BLOCK : OUTPUT_BLOCK + CROSSFADE]
    return aligned[:OUTPUT_BLOCK], offset


def percentile_summary(values: list[float], warmup: int) -> dict[str, float | int]:
    array = np.asarray(values, dtype=np.float64)
    steady = array[warmup:]
    if steady.size == 0:
        steady = array
    return {
        "samples": int(array.size),
        "warmup_excluded": min(warmup, max(int(array.size) - 1, 0)),
        "mean_ms": float(np.mean(steady)),
        "median_ms": float(np.median(steady)),
        "p95_ms": float(np.percentile(steady, 95)),
        "max_ms": float(np.max(steady)),
    }


def main() -> int:
    args = parse_arguments()
    hubert_path = require_file(args.hubert)
    rmvpe_path = require_file(args.rmvpe)
    generator_path = require_file(args.generator)
    mel_basis_path = require_file(args.mel_basis)
    voice_params_path = require_file(args.voice_params)
    input_path = require_file(args.input)

    mel_basis = np.load(mel_basis_path).astype(np.float32)
    if mel_basis.shape != (128, 513):
        raise ValueError(f"unexpected mel basis shape: {mel_basis.shape}")
    voice_params = json.loads(voice_params_path.read_text(encoding="utf-8"))
    linear_weight = float(voice_params["nsf_linear_weight"])
    linear_bias = float(voice_params["nsf_linear_bias"])

    input_audio = read_pcm16_mono(input_path)
    block_count = (input_audio.size + BLOCK_INPUT - 1) // BLOCK_INPUT
    if args.max_blocks > 0:
        block_count = min(block_count, args.max_blocks)
    padded_length = block_count * BLOCK_INPUT
    input_audio = np.pad(input_audio[:padded_length], (0, max(0, padded_length - input_audio.size)))

    core_masks = {
        "hubert": RKNNLite.NPU_CORE_0,
        "rmvpe": RKNNLite.NPU_CORE_1,
        "generator": RKNNLite.NPU_CORE_2,
    }
    sessions: list[RknnSession] = []
    load_started = time.perf_counter_ns()
    try:
        hubert = RknnSession(hubert_path, core_masks["hubert"], "hubert")
        sessions.append(hubert)
        rmvpe = RknnSession(rmvpe_path, core_masks["rmvpe"], "rmvpe")
        sessions.append(rmvpe)
        generator = RknnSession(
            generator_path, core_masks["generator"], "generator"
        )
        sessions.append(generator)
        load_ms = (time.perf_counter_ns() - load_started) / 1_000_000.0

        content_buffer = np.zeros(CONTENT_INPUT, dtype=np.float32)
        rmvpe_buffer = np.zeros(RMVPE_INPUT, dtype=np.float32)
        pitch_cache = np.zeros(F0_CACHE_SIZE, dtype=np.int64)
        pitchf_cache = np.zeros(F0_CACHE_SIZE, dtype=np.float32)
        sola_buffer = np.zeros(CROSSFADE, dtype=np.float32)
        fade_in = np.sin(
            0.5 * np.pi * np.linspace(0.0, 1.0, CROSSFADE, dtype=np.float32)
        ) ** 2
        rng = np.random.default_rng(args.seed)
        sid = np.zeros((1,), dtype=np.int64)

        timings: dict[str, list[float]] = {
            "hubert_rknn": [],
            "mel_cpu": [],
            "rmvpe_rknn": [],
            "f0_source_cpu": [],
            "generator_rknn": [],
            "sola_cpu": [],
            "block_total": [],
        }
        offsets: list[int] = []
        output_blocks: list[np.ndarray] = []
        run_started = time.perf_counter_ns()
        for block_index in range(block_count):
            block_started = time.perf_counter_ns()
            block = input_audio[
                block_index * BLOCK_INPUT : (block_index + 1) * BLOCK_INPUT
            ]
            content_buffer[:-BLOCK_INPUT] = content_buffer[BLOCK_INPUT:]
            content_buffer[-BLOCK_INPUT:] = block
            rmvpe_buffer[:-BLOCK_INPUT] = rmvpe_buffer[BLOCK_INPUT:]
            rmvpe_buffer[-BLOCK_INPUT:] = block

            started = time.perf_counter_ns()
            phone = np.asarray(
                hubert.infer([content_buffer[None].copy()])[0], dtype=np.float32
            )
            timings["hubert_rknn"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )
            if phone.shape != (1, CONTENT_FRAMES, 768):
                raise ValueError(f"unexpected HuBERT output shape: {phone.shape}")

            started = time.perf_counter_ns()
            mel = extract_mel(rmvpe_buffer, mel_basis)
            timings["mel_cpu"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

            started = time.perf_counter_ns()
            salience = np.asarray(rmvpe.infer([mel])[0], dtype=np.float32)
            timings["rmvpe_rknn"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

            started = time.perf_counter_ns()
            f0 = decode_rmvpe(salience)
            pitch, pitchf = update_pitch_cache(f0, pitch_cache, pitchf_cache)
            source = harmonic_source(
                pitchf, linear_weight, linear_bias, rng
            )
            noise = rng.standard_normal((1, 192, CONTENT_FRAMES)).astype(np.float32)
            timings["f0_source_cpu"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

            started = time.perf_counter_ns()
            generated_output = generator.infer(
                [phone, pitch, sid, noise, source]
            )[0]
            generated = np.asarray(generated_output, dtype=np.float32).reshape(-1)
            timings["generator_rknn"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

            started = time.perf_counter_ns()
            output_block, offset = sola_align(
                generated, sola_buffer, fade_in
            )
            timings["sola_cpu"].append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )
            offsets.append(offset)
            output_blocks.append(output_block)
            timings["block_total"].append(
                (time.perf_counter_ns() - block_started) / 1_000_000.0
            )
            print(
                f"block={block_index + 1}/{block_count} "
                f"total_ms={timings['block_total'][-1]:.3f} "
                f"generator_ms={timings['generator_rknn'][-1]:.3f} "
                f"sola_offset={offset}",
                flush=True,
            )
        run_ms = (time.perf_counter_ns() - run_started) / 1_000_000.0
    finally:
        for session in reversed(sessions):
            session.release()

    output_audio = np.concatenate(output_blocks)
    write_pcm16_mono(args.output, output_audio)
    input_duration_seconds = block_count * BLOCK_INPUT / CONTENT_RATE
    output_duration_seconds = output_audio.size / MODEL_RATE
    warmup = min(5, max(block_count - 1, 0))
    metrics = {
        "profile": "rvc-v2-40k-50ms-fixed",
        "input": str(input_path),
        "output": str(args.output.resolve()),
        "input_samples_16k": int(block_count * BLOCK_INPUT),
        "output_samples_40k": int(output_audio.size),
        "input_duration_seconds": input_duration_seconds,
        "output_duration_seconds": output_duration_seconds,
        "blocks": block_count,
        "block_audio_ms": 50,
        "crossfade_ms": 20,
        "sola_search_ms": 10,
        "pitch_shift_semitones": 0,
        "index_rate": 0.0,
        "protect": None,
        "seed": args.seed,
        "model_load_ms": load_ms,
        "run_wall_ms": run_ms,
        "run_rtf": run_ms / (input_duration_seconds * 1000.0),
        "npu_core_assignment": {
            "hubert": 0,
            "rmvpe": 1,
            "generator": 2,
        },
        "timings": {
            label: percentile_summary(values, warmup)
            for label, values in timings.items()
        },
        "sola_offset_samples": {
            "median": float(np.median(offsets)),
            "p95": float(np.percentile(offsets, 95)),
            "max": int(np.max(offsets)),
        },
        "output_peak": float(np.max(np.abs(output_audio))),
        "output_rms": float(np.sqrt(np.mean(output_audio * output_audio))),
    }
    args.metrics.parent.mkdir(parents=True, exist_ok=True)
    args.metrics.write_text(
        json.dumps(metrics, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metrics, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
