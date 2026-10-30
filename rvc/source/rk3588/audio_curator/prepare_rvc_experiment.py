#!/usr/bin/env python3

import argparse
import json
import random
from pathlib import Path

import numpy as np


def stems(directory, suffix):
    return {path.name[: -len(suffix)] for path in directory.glob(f"*{suffix}")}


def parse_arguments():
    parser = argparse.ArgumentParser(description="Prepare a single-speaker RVC experiment.")
    parser.add_argument("--experiment", required=True)
    parser.add_argument("--config-template", required=True)
    parser.add_argument("--speaker-id", type=int, default=0)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    experiment = Path(arguments.experiment).resolve()
    gt_dir = experiment / "0_gt_wavs"
    feature_dir = experiment / "3_feature768"
    f0_dir = experiment / "2a_f0"
    f0nsf_dir = experiment / "2b-f0nsf"

    gt_names = stems(gt_dir, ".wav")
    feature_names = stems(feature_dir, ".npy")
    f0_names = stems(f0_dir, ".wav.npy")
    f0nsf_names = stems(f0nsf_dir, ".wav.npy")
    names = sorted(gt_names & feature_names & f0_names & f0nsf_names)
    if not names:
        raise RuntimeError("no complete training records found")

    missing = {
        "features": sorted(gt_names - feature_names),
        "f0": sorted(gt_names - f0_names),
        "f0nsf": sorted(gt_names - f0nsf_names),
    }
    if any(missing.values()):
        raise RuntimeError(f"incomplete training records: {missing}")

    lines = []
    feature_lengths = []
    for name in names:
        feature_path = feature_dir / f"{name}.npy"
        feature_lengths.append(int(np.load(feature_path, mmap_mode="r").shape[0] * 2))
        lines.append(
            "|".join(
                [
                    str(gt_dir / f"{name}.wav"),
                    str(feature_path),
                    str(f0_dir / f"{name}.wav.npy"),
                    str(f0nsf_dir / f"{name}.wav.npy"),
                    str(arguments.speaker_id),
                ]
            )
        )

    random.Random(1234).shuffle(lines)
    (experiment / "filelist.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    config = json.loads(Path(arguments.config_template).read_text(encoding="utf-8"))
    config.pop("speaker_info", None)
    (experiment / "config.json").write_text(
        json.dumps(config, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    summary = {
        "records": len(names),
        "speaker_id": arguments.speaker_id,
        "feature_length_min": min(feature_lengths),
        "feature_length_max": max(feature_lengths),
        "records_at_or_below_old_100_frame_boundary": sum(
            length <= 100 for length in feature_lengths
        ),
    }
    (experiment / "preparation-summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
