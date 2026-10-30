#!/usr/bin/env python3

import argparse
import concurrent.futures
import hashlib
import html
import json
import re
import subprocess
import time
import urllib.request
from pathlib import Path


PAGE_URL = "https://wiki.biligame.com/ys/%E8%8A%99%E5%AE%81%E5%A8%9C%E8%AF%AD%E9%9F%B3"
RAW_URL = "https://wiki.biligame.com/ys/index.php?title=%E8%8A%99%E5%AE%81%E5%A8%9C%E8%AF%AD%E9%9F%B3&action=raw"
USER_AGENT = "Mozilla/5.0 (compatible; RVC-dataset-research/1.0)"


def fetch_bytes(url, timeout=60):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def unique_in_order(values):
    result = []
    seen = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def parse_sources(page_html, raw_text):
    raw_urls = re.findall(r'data-src="([^"]+\.mp3)"', page_html)
    urls = unique_in_order(
        html.unescape(value).replace("&#58;", ":") for value in raw_urls
    )
    if not urls or len(urls) % 4 != 0:
        raise RuntimeError(f"unexpected audio URL count: {len(urls)}")

    chinese_urls = urls[0::4]
    titles = re.findall(r"^\|语音类型=(.+)$", raw_text, re.MULTILINE)
    transcripts = re.findall(r"^\|语音内容=(.*)$", raw_text, re.MULTILINE)
    if len(titles) < len(chinese_urls) or len(transcripts) < len(chinese_urls):
        raise RuntimeError(
            "raw page has fewer labels than Chinese audio files: "
            f"titles={len(titles)} transcripts={len(transcripts)} audio={len(chinese_urls)}"
        )

    entries = []
    for index, url in enumerate(chinese_urls):
        entries.append(
            {
                "index": index,
                "title": titles[index],
                "transcript": transcripts[index],
                "source_url": url,
                "source_name": f"{index:03d}.ogg",
                "training_name": f"{index:03d}.wav",
            }
        )
    return entries


def probe_audio(path):
    command = [
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format=duration,size,bit_rate:stream=codec_name,sample_rate,channels,bit_rate",
        "-of",
        "json",
        str(path),
    ]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    payload = json.loads(completed.stdout)
    stream = payload["streams"][0]
    audio_format = payload["format"]
    return {
        "codec": stream.get("codec_name"),
        "sample_rate": int(stream.get("sample_rate", 0)),
        "channels": int(stream.get("channels", 0)),
        "stream_bit_rate": int(stream.get("bit_rate", 0) or 0),
        "duration_seconds": float(audio_format.get("duration", 0)),
        "size_bytes": int(audio_format.get("size", 0)),
        "container_bit_rate": int(audio_format.get("bit_rate", 0) or 0),
    }


def download_one(entry, original_dir):
    destination = original_dir / entry["source_name"]
    if destination.is_file() and destination.stat().st_size > 0:
        content = destination.read_bytes()
        if content.startswith(b"OggS"):
            return destination

    last_error = None
    for attempt in range(1, 4):
        try:
            content = fetch_bytes(entry["source_url"])
            if not content.startswith(b"OggS"):
                raise RuntimeError("downloaded file is not an Ogg stream")
            temporary = destination.with_suffix(".ogg.part")
            temporary.write_bytes(content)
            temporary.replace(destination)
            return destination
        except Exception as error:
            last_error = error
            if attempt < 3:
                time.sleep(attempt * 2)
    raise RuntimeError(f"failed to download {entry['source_url']}: {last_error}")


def convert_one(entry, original_dir, training_dir):
    source = original_dir / entry["source_name"]
    destination = training_dir / entry["training_name"]
    command = [
        "ffmpeg",
        "-nostdin",
        "-v",
        "error",
        "-y",
        "-i",
        str(source),
        "-ac",
        "1",
        "-ar",
        "32000",
        "-c:a",
        "pcm_s16le",
        str(destination),
    ]
    subprocess.run(command, check=True)
    return destination


def write_sha256(paths, destination):
    lines = []
    for path in sorted(paths):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.name}\n")
    destination.write_text("".join(lines), encoding="utf-8")


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Download all available Chinese Furina voice clips from Bilibili BWIKI."
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--workers", type=int, default=8)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    output_root = Path(arguments.output).resolve()
    original_dir = output_root / "original-cn-ogg"
    training_dir = output_root / "training-32k-wav"
    original_dir.mkdir(parents=True, exist_ok=True)
    training_dir.mkdir(parents=True, exist_ok=True)

    page_bytes = fetch_bytes(PAGE_URL)
    raw_bytes = fetch_bytes(RAW_URL)
    page_text = page_bytes.decode("utf-8")
    raw_text = raw_bytes.decode("utf-8")
    entries = parse_sources(page_text, raw_text)

    (output_root / "source-page.html").write_bytes(page_bytes)
    (output_root / "source-page.raw.txt").write_bytes(raw_bytes)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, arguments.workers)) as executor:
        original_paths = list(
            executor.map(lambda entry: download_one(entry, original_dir), entries)
        )

    for entry, path in zip(entries, original_paths):
        entry["source_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        entry["source_audio"] = probe_audio(path)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, arguments.workers)) as executor:
        training_paths = list(
            executor.map(
                lambda entry: convert_one(entry, original_dir, training_dir), entries
            )
        )

    for entry, path in zip(entries, training_paths):
        entry["training_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        entry["training_audio"] = probe_audio(path)

    total_duration = sum(entry["source_audio"]["duration_seconds"] for entry in entries)
    manifest = {
        "source_page": PAGE_URL,
        "source_raw_page": RAW_URL,
        "language": "zh-CN",
        "character": "芙宁娜",
        "entry_count": len(entries),
        "total_duration_seconds": total_duration,
        "entries": entries,
    }
    (output_root / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    write_sha256(original_paths, output_root / "original-SHA256SUMS")
    write_sha256(training_paths, output_root / "training-SHA256SUMS")

    print(f"Downloaded and converted {len(entries)} Chinese clips")
    print(f"Total duration: {total_duration:.3f} seconds ({total_duration / 60:.3f} minutes)")
    print(f"Output: {output_root}")


if __name__ == "__main__":
    main()
