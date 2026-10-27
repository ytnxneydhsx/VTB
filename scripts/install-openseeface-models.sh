#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
model_dir="$project_root/models/openseeface"
base_url="https://raw.githubusercontent.com/emilianavt/OpenSeeFace/v1.20.2/models"

mkdir -p "$model_dir"

download_model() {
    filename=$1
    expected_sha256=$2
    destination="$model_dir/$filename"

    if [ -f "$destination" ]; then
        actual_sha256=$(shasum -a 256 "$destination" | awk '{print $1}')
        if [ "$actual_sha256" = "$expected_sha256" ]; then
            printf '%s: already installed and verified\n' "$filename"
            return
        fi
        printf '%s: checksum mismatch, downloading a fresh copy\n' "$filename"
    fi

    temporary_file="$destination.download"
    rm -f "$temporary_file"
    curl --fail --location --retry 3 --output "$temporary_file" "$base_url/$filename"

    actual_sha256=$(shasum -a 256 "$temporary_file" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        rm -f "$temporary_file"
        printf '%s: SHA-256 verification failed\n' "$filename" >&2
        exit 1
    fi

    mv "$temporary_file" "$destination"
    printf '%s: installed and verified\n' "$filename"
}

download_model \
    "mnv3_detection_opt.onnx" \
    "0e8e4806766d85ab067a52c7af0dcb59eb7f9dfe580b44f20a8e6ab712d89809"

download_model \
    "lm_model3_opt.onnx" \
    "5aa0ab2594acaf2e1d3916a8de592cffeb1f1a7a7f778cad2f37948bff6549d1"
