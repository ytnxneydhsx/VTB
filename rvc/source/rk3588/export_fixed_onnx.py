#!/usr/bin/env python3
"""Export the fixed RVC v2 32/40 kHz realtime graphs to ONNX.

The official pretrained generator is useful only for probing graph conversion.
For an actual voice, pass an RVC v2 inference checkpoint containing the usual
``config`` and ``weight`` keys.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
import onnx
import onnxruntime as ort
import torch
from torch import nn
from torch.nn import functional as F

from fixed_profile import (
    CONTENT_FEATURES,
    CONTENT_FRAMES,
    CONTENT_INPUT_SAMPLES,
    LATENT_CHANNELS,
    RETURN_FRAMES,
    RMVPE_BINS,
    RMVPE_FRAMES,
    RMVPE_MELS,
    SKIP_HEAD_FRAMES,
    generator_output_samples,
    model_sample_rate_label,
    normalize_model_sample_rate,
)


class HubertRealtimeWrapper(nn.Module):
    """Return the exact v2 phone tensor consumed by the realtime generator."""

    def __init__(self, model: nn.Module) -> None:
        super().__init__()
        self.model = model

    def forward(self, input_values: torch.Tensor) -> torch.Tensor:
        hidden = self.model(
            input_values=input_values,
            attention_mask=None,
            output_hidden_states=False,
            return_dict=False,
        )[0]
        hidden = torch.cat((hidden, hidden[:, -1:, :]), dim=1)
        hidden = hidden.repeat_interleave(2, dim=1)
        return hidden[:, :CONTENT_FRAMES, :]


class GeneratorExternalSourceDecoder(nn.Module):
    """RVC NSF decoder with the harmonic source supplied by the host."""

    def __init__(self, decoder: nn.Module) -> None:
        super().__init__()
        self.decoder = decoder

    def forward(
        self,
        latent: torch.Tensor,
        harmonic_source: torch.Tensor,
        speaker_condition: torch.Tensor,
    ) -> torch.Tensor:
        x = self.decoder.conv_pre(latent)
        x = x + self.decoder.cond(speaker_condition)
        for index in range(self.decoder.num_upsamples):
            x = F.leaky_relu(x, self.decoder.lrelu_slope)
            x = self.decoder.ups[index](x)
            x = x + self.decoder.noise_convs[index](harmonic_source)
            combined = self.decoder.resblocks[
                index * self.decoder.num_kernels
            ](x)
            for kernel_index in range(1, self.decoder.num_kernels):
                combined = combined + self.decoder.resblocks[
                    index * self.decoder.num_kernels + kernel_index
                ](x)
            x = combined / self.decoder.num_kernels
        x = F.leaky_relu(x)
        x = self.decoder.conv_post(x)
        return torch.tanh(x)


class GeneratorRealtimeWrapper(nn.Module):
    """Fixed 13-frame RVC generator producing eight output frames."""

    def __init__(self, model: nn.Module) -> None:
        super().__init__()
        self.emb_g = model.emb_g
        self.enc_p = model.enc_p
        self.flow = model.flow
        self.decoder = GeneratorExternalSourceDecoder(model.dec)
        self.register_buffer(
            "phone_lengths", torch.tensor([CONTENT_FRAMES], dtype=torch.long)
        )

    def forward(
        self,
        phone: torch.Tensor,
        pitch: torch.Tensor,
        sid: torch.Tensor,
        noise: torch.Tensor,
        harmonic_source: torch.Tensor,
    ) -> torch.Tensor:
        speaker_condition = self.emb_g(sid).unsqueeze(-1)
        mean, logs, mask = self.enc_p(
            phone,
            pitch,
            self.phone_lengths,
        )
        latent = (
            mean + torch.exp(logs) * noise * 0.66666
        ) * mask
        latent = self.flow(
            latent,
            mask,
            g=speaker_condition,
            reverse=True,
        )
        latent = latent[:, :, SKIP_HEAD_FRAMES : SKIP_HEAD_FRAMES + RETURN_FRAMES]
        mask = mask[:, :, SKIP_HEAD_FRAMES : SKIP_HEAD_FRAMES + RETURN_FRAMES]
        return self.decoder(
            latent * mask,
            harmonic_source,
            speaker_condition,
        )


def fuse_conv_transpose_batch_norm(
    convolution: nn.ConvTranspose2d,
    batch_norm: nn.BatchNorm2d,
) -> nn.ConvTranspose2d:
    """Fold an evaluation BatchNorm2d into a ConvTranspose2d."""

    if convolution.out_channels != batch_norm.num_features:
        raise ValueError("ConvTranspose2d and BatchNorm2d channel counts differ")
    if batch_norm.running_mean is None or batch_norm.running_var is None:
        raise ValueError("BatchNorm2d must track running statistics")

    fused = nn.ConvTranspose2d(
        in_channels=convolution.in_channels,
        out_channels=convolution.out_channels,
        kernel_size=convolution.kernel_size,
        stride=convolution.stride,
        padding=convolution.padding,
        output_padding=convolution.output_padding,
        groups=convolution.groups,
        bias=True,
        dilation=convolution.dilation,
        padding_mode=convolution.padding_mode,
        device=convolution.weight.device,
        dtype=convolution.weight.dtype,
    )

    if batch_norm.affine:
        gamma = batch_norm.weight
        beta = batch_norm.bias
    else:
        gamma = torch.ones_like(batch_norm.running_mean)
        beta = torch.zeros_like(batch_norm.running_mean)
    scale = gamma / torch.sqrt(batch_norm.running_var + batch_norm.eps)

    groups = convolution.groups
    input_channels_per_group = convolution.in_channels // groups
    output_channels_per_group = convolution.out_channels // groups
    weight = convolution.weight.reshape(
        groups,
        input_channels_per_group,
        output_channels_per_group,
        *convolution.kernel_size,
    )
    scaled_weight = weight * scale.reshape(
        groups,
        1,
        output_channels_per_group,
        1,
        1,
    )

    if convolution.bias is None:
        convolution_bias = torch.zeros_like(batch_norm.running_mean)
    else:
        convolution_bias = convolution.bias
    fused_bias = beta + (convolution_bias - batch_norm.running_mean) * scale

    with torch.no_grad():
        fused.weight.copy_(scaled_weight.reshape_as(fused.weight))
        fused.bias.copy_(fused_bias)
    return fused


class RknnCompatibleConvTranspose2d(nn.Module):
    """Express output padding as an equivalent crop of a larger deconvolution."""

    def __init__(self, convolution: nn.ConvTranspose2d) -> None:
        super().__init__()
        padding_height, padding_width = convolution.padding
        output_padding_height, output_padding_width = convolution.output_padding
        if output_padding_height > padding_height:
            raise ValueError("unsupported ConvTranspose2d height output padding")
        if output_padding_width > padding_width:
            raise ValueError("unsupported ConvTranspose2d width output padding")
        self.crop_top = padding_height
        self.crop_left = padding_width
        self.crop_bottom = padding_height - output_padding_height
        self.crop_right = padding_width - output_padding_width
        self.convolution = nn.ConvTranspose2d(
            in_channels=convolution.in_channels,
            out_channels=convolution.out_channels,
            kernel_size=convolution.kernel_size,
            stride=convolution.stride,
            padding=0,
            output_padding=0,
            groups=convolution.groups,
            bias=convolution.bias is not None,
            dilation=convolution.dilation,
            padding_mode=convolution.padding_mode,
            device=convolution.weight.device,
            dtype=convolution.weight.dtype,
        )
        with torch.no_grad():
            self.convolution.weight.copy_(convolution.weight)
            if convolution.bias is not None:
                self.convolution.bias.copy_(convolution.bias)

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        output = self.convolution(inputs)
        height_end = output.shape[2] - self.crop_bottom
        width_end = output.shape[3] - self.crop_right
        return output[
            :,
            :,
            self.crop_top : height_end,
            self.crop_left : width_end,
        ]


def fold_rmvpe_decoder_batch_norm(model: nn.Module) -> int:
    """Rewrite RMVPE decoder deconvolutions for RKNN Toolkit2."""

    folded = 0
    for layer in model.unet.decoder.layers:
        if len(layer.conv1) < 3:
            raise ValueError("unexpected RMVPE decoder conv1 layout")
        convolution = layer.conv1[0]
        batch_norm = layer.conv1[1]
        activation = layer.conv1[2]
        if not isinstance(convolution, nn.ConvTranspose2d):
            raise TypeError("RMVPE decoder conv1[0] is not ConvTranspose2d")
        if not isinstance(batch_norm, nn.BatchNorm2d):
            raise TypeError("RMVPE decoder conv1[1] is not BatchNorm2d")
        fused = fuse_conv_transpose_batch_norm(convolution, batch_norm)
        layer.conv1 = nn.Sequential(RknnCompatibleConvTranspose2d(fused), activation)
        folded += 1
    return folded


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--hubert-dir", type=Path, required=True)
    parser.add_argument("--rmvpe-checkpoint", type=Path, required=True)
    parser.add_argument("--generator-checkpoint", type=Path, required=True)
    parser.add_argument(
        "--generator-sample-rate",
        choices=("auto", "32k", "40k"),
        default="auto",
        help=(
            "validate an inference checkpoint rate, or select the structure "
            "for a pretrained generator probe"
        ),
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--model",
        choices=("all", "hubert", "rmvpe", "generator"),
        default="all",
    )
    parser.add_argument("--opset", type=int, default=17)
    return parser.parse_args()


def prepare_repo_imports(repo_root: Path) -> None:
    repo_text = str(repo_root.resolve())
    if repo_text not in sys.path:
        sys.path.insert(0, repo_text)


def export_model(
    model: nn.Module,
    example_inputs: tuple[torch.Tensor, ...],
    output_path: Path,
    input_names: list[str],
    output_name: str,
    opset: int,
) -> np.ndarray:
    model.eval()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        torch_output = model(*example_inputs).detach().cpu().numpy()
    torch.onnx.export(
        model,
        example_inputs,
        str(output_path),
        export_params=True,
        opset_version=opset,
        do_constant_folding=True,
        input_names=input_names,
        output_names=[output_name],
        dynamic_axes=None,
    )
    onnx_model = onnx.load(str(output_path))
    onnx.checker.check_model(onnx_model)
    inferred = onnx.shape_inference.infer_shapes(onnx_model)
    onnx.save(inferred, str(output_path))

    session = ort.InferenceSession(
        str(output_path),
        providers=["CPUExecutionProvider"],
    )
    feed = {}
    for name, tensor in zip(input_names, example_inputs):
        feed[name] = tensor.detach().cpu().numpy()
    ort_output = session.run([output_name], feed)[0]
    if ort_output.shape != torch_output.shape:
        raise RuntimeError(
            f"{output_path.name}: shape mismatch "
            f"PyTorch={torch_output.shape} ONNX={ort_output.shape}"
        )
    difference = np.abs(ort_output.astype(np.float32) - torch_output.astype(np.float32))
    metrics = {
        "shape": list(ort_output.shape),
        "max_abs_error": float(difference.max(initial=0.0)),
        "mean_abs_error": float(difference.mean()),
    }
    metrics_path = output_path.with_suffix(".validation.json")
    metrics_path.write_text(json.dumps(metrics, indent=2) + "\n")
    print(f"exported={output_path}")
    print(f"validation={metrics_path} {metrics}")
    return ort_output


def export_hubert(args: argparse.Namespace) -> None:
    from transformers import HubertModel

    model = HubertModel.from_pretrained(
        str(args.hubert_dir.resolve()),
        local_files_only=True,
        attn_implementation="eager",
    ).float()
    wrapper = HubertRealtimeWrapper(model)
    input_values = torch.linspace(
        -0.2,
        0.2,
        CONTENT_INPUT_SAMPLES,
        dtype=torch.float32,
    ).reshape(1, CONTENT_INPUT_SAMPLES)
    export_model(
        wrapper,
        (input_values,),
        args.output_dir / "rvc_v2_hubert_2080.onnx",
        ["input_values"],
        "phone",
        args.opset,
    )


def export_rmvpe(args: argparse.Namespace) -> None:
    from infer.rmvpe import E2E

    model = E2E(4, 1, (2, 2))
    checkpoint = torch.load(
        args.rmvpe_checkpoint,
        map_location="cpu",
        weights_only=False,
    )
    model.load_state_dict(checkpoint, strict=True)
    model = model.float().eval()
    mel = torch.linspace(
        -6.0,
        2.0,
        RMVPE_MELS * RMVPE_FRAMES,
        dtype=torch.float32,
    ).reshape(1, RMVPE_MELS, RMVPE_FRAMES)
    with torch.no_grad():
        reference = model(mel).detach().cpu().numpy()
    folded = fold_rmvpe_decoder_batch_norm(model)
    with torch.no_grad():
        folded_output = model(mel).detach().cpu().numpy()
    folding_difference = np.abs(
        folded_output.astype(np.float32) - reference.astype(np.float32)
    )
    folding_max_error = float(folding_difference.max(initial=0.0))
    folding_mean_error = float(folding_difference.mean())
    if folding_max_error > 1e-5:
        raise RuntimeError(
            "RMVPE decoder BatchNorm folding changed output: "
            f"max_abs_error={folding_max_error}"
        )
    print(
        "rmvpe_decoder_batch_norm_folded="
        f"{folded} max_abs_error={folding_max_error} "
        f"mean_abs_error={folding_mean_error}"
    )
    output = export_model(
        model,
        (mel,),
        args.output_dir / "rvc_rmvpe_32frames.onnx",
        ["mel"],
        "salience",
        args.opset,
    )
    expected = (1, RMVPE_FRAMES, RMVPE_BINS)
    if output.shape != expected:
        raise RuntimeError(f"RMVPE output shape {output.shape}, expected {expected}")


def load_generator_from_inference_checkpoint(
    checkpoint: dict[str, Any],
) -> tuple[nn.Module, str, int]:
    from infer.module.models import SynthesizerTrnMs768NSFsid

    if checkpoint.get("version", "v1") != "v2":
        raise ValueError("generator checkpoint must be RVC v2")
    if checkpoint.get("f0", 1) != 1:
        raise ValueError("generator checkpoint must contain the F0 model")
    config = list(checkpoint["config"])
    config[-3] = checkpoint["weight"]["emb_g.weight"].shape[0]
    sample_rate = normalize_model_sample_rate(config[-1])
    model = SynthesizerTrnMs768NSFsid(*config, is_half=False)
    model.load_state_dict(checkpoint["weight"], strict=False)
    return model, "inference", sample_rate


def load_generator_from_pretrain(
    checkpoint: dict[str, Any],
    repo_root: Path,
    sample_rate: int,
) -> tuple[nn.Module, str, int]:
    from infer.module.models import SynthesizerTrnMs768NSFsid

    config_paths = {
        32_000: repo_root / "configs" / "v2" / "32k.json",
        40_000: repo_root / "configs" / "v1" / "40k.json",
    }
    config_path = config_paths[sample_rate]
    config = json.loads(config_path.read_text())
    data = config["data"]
    model_config = config["model"]
    model = SynthesizerTrnMs768NSFsid(
        data["filter_length"] // 2 + 1,
        config["train"]["segment_size"] // data["hop_length"],
        model_config["inter_channels"],
        model_config["hidden_channels"],
        model_config["filter_channels"],
        model_config["n_heads"],
        model_config["n_layers"],
        model_config["kernel_size"],
        model_config["p_dropout"],
        model_config["resblock"],
        model_config["resblock_kernel_sizes"],
        model_config["resblock_dilation_sizes"],
        model_config["upsample_rates"],
        model_config["upsample_initial_channel"],
        model_config["upsample_kernel_sizes"],
        model_config["spk_embed_dim"],
        model_config["gin_channels"],
        model_sample_rate_label(sample_rate),
        is_half=False,
    )
    model.load_state_dict(checkpoint["model"], strict=True)
    return model, "pretrained-structure-probe", sample_rate


def load_generator(
    checkpoint_path: Path,
    repo_root: Path,
    requested_sample_rate: int | None,
) -> tuple[nn.Module, str, int]:
    checkpoint = torch.load(
        checkpoint_path,
        map_location="cpu",
        weights_only=True,
    )
    if "weight" in checkpoint and "config" in checkpoint:
        model, source_kind, sample_rate = load_generator_from_inference_checkpoint(
            checkpoint
        )
        if requested_sample_rate is not None and sample_rate != requested_sample_rate:
            raise ValueError(
                "generator checkpoint sample rate mismatch: "
                f"checkpoint={model_sample_rate_label(sample_rate)} "
                f"requested={model_sample_rate_label(requested_sample_rate)}"
            )
    elif "model" in checkpoint:
        if requested_sample_rate is None:
            raise ValueError(
                "a pretrained structure probe requires --generator-sample-rate"
            )
        model, source_kind, sample_rate = load_generator_from_pretrain(
            checkpoint,
            repo_root,
            requested_sample_rate,
        )
    else:
        raise ValueError("unrecognized RVC generator checkpoint format")
    if hasattr(model, "enc_q"):
        del model.enc_q
    model = model.float().eval()
    model.remove_weight_norm()
    return model, source_kind, sample_rate


def export_generator(args: argparse.Namespace) -> None:
    torch.manual_seed(20261029)
    requested_sample_rate = None
    if args.generator_sample_rate != "auto":
        requested_sample_rate = normalize_model_sample_rate(
            args.generator_sample_rate
        )
    model, source_kind, sample_rate = load_generator(
        args.generator_checkpoint,
        args.repo_root,
        requested_sample_rate,
    )
    output_samples = generator_output_samples(sample_rate)
    sample_rate_label = model_sample_rate_label(sample_rate)
    wrapper = GeneratorRealtimeWrapper(model).float().eval()
    phone = torch.randn(1, CONTENT_FRAMES, CONTENT_FEATURES, dtype=torch.float32)
    pitch = torch.randint(1, 255, (1, CONTENT_FRAMES), dtype=torch.long)
    sid = torch.zeros(1, dtype=torch.long)
    noise = torch.randn(
        1,
        LATENT_CHANNELS,
        CONTENT_FRAMES,
        dtype=torch.float32,
    )
    harmonic_source = torch.randn(
        1,
        1,
        output_samples,
        dtype=torch.float32,
    ) * 0.03
    output_path = (
        args.output_dir / f"rvc_v2_{sample_rate_label}_generator_13to8.onnx"
    )
    output = export_model(
        wrapper,
        (phone, pitch, sid, noise, harmonic_source),
        output_path,
        ["phone", "pitch", "sid", "noise", "har_source"],
        "audio",
        args.opset,
    )
    expected = (1, 1, output_samples)
    if output.shape != expected:
        raise RuntimeError(f"generator output shape {output.shape}, expected {expected}")
    metadata_path = output_path.with_suffix(".metadata.json")
    metadata = {
        "checkpoint": str(args.generator_checkpoint.resolve()),
        "checkpoint_kind": source_kind,
        "deployable_voice": source_kind == "inference",
        "sample_rate": sample_rate,
        "sample_rate_label": sample_rate_label,
        "content_frames": CONTENT_FRAMES,
        "skip_head_frames": SKIP_HEAD_FRAMES,
        "return_frames": RETURN_FRAMES,
        "output_samples": output_samples,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"metadata={metadata_path} {metadata}")


def main() -> None:
    args = parse_arguments()
    args.repo_root = args.repo_root.resolve()
    args.hubert_dir = args.hubert_dir.resolve()
    args.rmvpe_checkpoint = args.rmvpe_checkpoint.resolve()
    args.generator_checkpoint = args.generator_checkpoint.resolve()
    args.output_dir = args.output_dir.resolve()
    prepare_repo_imports(args.repo_root)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    if args.model in ("all", "hubert"):
        export_hubert(args)
    if args.model in ("all", "rmvpe"):
        export_rmvpe(args)
    if args.model in ("all", "generator"):
        export_generator(args)


if __name__ == "__main__":
    main()
