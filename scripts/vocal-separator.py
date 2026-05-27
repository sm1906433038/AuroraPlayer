#!/usr/bin/env python3
"""
vocal-separator.py — Extract vocals from an audio file using BS-RoFormer.

Called by AuroraPlayer's TranscriptionJob as a pre-processing step before whisper.
Requires the `audio-separator[gpu]` package (auto-installed on first use
via the --ensure-deps flag).

Usage:
    python vocal-separator.py INPUT_WAV OUTPUT_DIR [--model MODEL] [--ensure-deps]

Outputs:
    <OUTPUT_DIR>/<stem>_(Vocals).wav   — isolated vocal track
    <OUTPUT_DIR>/<stem>_(Instrumental).wav — everything else (discarded by caller)

Exit code 0 on success; the vocal file path is printed to stdout as the
last line so the C++ caller can capture it with QProcess::readAllStandardOutput.
"""

import argparse
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_MODEL = "model_bs_roformer_ep_317_sdr_12.9755.ckpt"


def ensure_deps(venv_pip: str | None = None) -> None:
    """Install audio-separator[gpu] + onnxruntime-gpu if missing."""
    pip = venv_pip or sys.executable
    pip_cmd = [pip, "-m", "pip"] if venv_pip is None else [pip]

    if importlib.util.find_spec("separator") is not None:
        return

    print("[vocal-separator] Installing audio-separator[gpu]...", flush=True)
    # audio-separator pulls in torch etc. if needed; the [gpu] extra
    # selects onnxruntime-gpu for CUDA acceleration.
    subprocess.check_call(
        [sys.executable, "-m", "pip", "install",
         "audio-separator[gpu]"],
        stdout=sys.stderr,  # keep stdout clean for the path output
    )


def separate(input_path: str, output_dir: str, model: str) -> str:
    """Run separation and return the path to the vocals file."""
    from audio_separator.separator import Separator

    model_dir = str(Path(__file__).resolve().parent.parent / ".cache" / "audio-separator-models")
    sep = Separator(
        model_file_dir=model_dir,
        output_dir=output_dir,
        output_format="WAV",
        output_bitrate=None,
        normalization_threshold=0.9,
        output_single_stem="Vocals",
    )

    print(f"[vocal-separator] Loading model: {model}", flush=True)
    sep.load_model(model)

    print(f"[vocal-separator] Separating: {input_path}", flush=True)
    output_files = sep.separate(input_path)

    # output_files is a list of path strings. With output_single_stem="Vocals"
    # there should be exactly one file — the vocals.
    if not output_files:
        raise RuntimeError("audio-separator returned no output files")

    vocals_path = output_files[0]
    # audio-separator may return just a filename without the output_dir prefix.
    if not os.path.isabs(vocals_path):
        vocals_path = os.path.join(output_dir, vocals_path)
    if not os.path.exists(vocals_path):
        raise RuntimeError(f"Vocals file not found: {vocals_path}")

    # BS-RoFormer internally resamples to 44.1 kHz. Whisper needs 16 kHz
    # mono — resample back so the C++ WAV loader doesn't reject it.
    import soundfile as sf
    import numpy as np

    data, sr = sf.read(vocals_path)
    if sr != 16000 or data.ndim > 1:
        print(f"[vocal-separator] Resampling {sr} Hz → 16000 Hz mono", flush=True)
        # Mix to mono if stereo.
        if data.ndim > 1:
            data = data.mean(axis=1)
        if sr != 16000:
            import resampy
            data = resampy.resample(data, sr, 16000)
        resampled_path = os.path.join(output_dir, "vocals_16k.wav")
        sf.write(resampled_path, data.astype(np.float32), 16000, subtype='PCM_16')
        os.remove(vocals_path)
        vocals_path = resampled_path

    print(f"[vocal-separator] Done: {vocals_path}", flush=True)
    return vocals_path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract vocals from audio using BS-RoFormer.")
    ap.add_argument("input", help="Input audio file (WAV)")
    ap.add_argument("output_dir", help="Directory for output files")
    ap.add_argument("--model", default=DEFAULT_MODEL,
                    help=f"Model checkpoint name (default: {DEFAULT_MODEL})")
    ap.add_argument("--ensure-deps", action="store_true",
                    help="Auto-install audio-separator if missing")
    args = ap.parse_args()

    if args.ensure_deps:
        ensure_deps()

    os.makedirs(args.output_dir, exist_ok=True)
    vocals = separate(args.input, args.output_dir, args.model)

    # The LAST line of stdout is the vocals path — C++ reads this.
    print(vocals)
    return 0


if __name__ == "__main__":
    sys.exit(main())
