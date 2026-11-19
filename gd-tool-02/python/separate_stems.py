"""gd-tool-02 stem separator (spike). Adapted from dream-charter-engine's
ai_charter/separation.py.

    python separate_stems.py SONG.mp3 OUT_DIR [--mode demucs|simple]

Writes OUT_DIR/<stem>.wav (16-bit PCM) and prints one line per event on stdout,
which the C++ window reads:
    STATUS <text>
    STEM <name> <path>
    PROGRESS <0..1>
    DONE
    ERROR <text>
The MP3 is decoded with libsndfile (soundfile), so ffmpeg is not needed.

Fully offline: the Demucs weights are read from gd-tool-02/models/
(<model>.yaml + one .safetensors per model in the bag), never downloaded.
"""

from __future__ import annotations

import argparse
import os
import sys

# Never reach the network (Hugging Face, torch hub), even by accident.
os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"
from pathlib import Path

import numpy as np
import soundfile as sf

MODELS_DIR = Path(__file__).resolve().parent.parent / "models"

# Demucs htdemucs_6s names -> ours (the launcher's instruments).
DEMUCS_NAMES = {"drums": "drums", "bass": "bass", "other": "other",
                "vocals": "vocals", "guitar": "guitar", "piano": "keys"}


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


def load_audio(path: Path) -> tuple[np.ndarray, int]:
    audio, sr = sf.read(str(path), dtype="float32", always_2d=True)  # (frames, channels)
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)
    return audio[:, :2], sr


def write_stem(out_dir: Path, name: str, audio: np.ndarray, sr: int) -> None:
    path = out_dir / f"{name}.wav"
    sf.write(str(path), np.clip(audio, -1.0, 1.0), sr, subtype="PCM_16")
    emit("STEM", name, str(path))


def load_local_model(name: str):
    """The same model demucs.pretrained.get_model() would download, read from
    MODELS_DIR (copied from the Hugging Face repo adefossez/HTDemucs-6s)."""
    import yaml
    from demucs.apply import BagOfModels
    from demucs.hf import load_safetensors_model

    bag_file = MODELS_DIR / f"{name}.yaml"
    if not bag_file.exists():
        raise FileNotFoundError(f"{bag_file} is missing; see README (Models).")
    bag = yaml.safe_load(bag_file.read_text())
    models = []
    for sig in bag["models"]:
        weights = MODELS_DIR / f"{sig}.safetensors"
        if not weights.exists():
            raise FileNotFoundError(f"{weights} is missing; see README (Models).")
        models.append(load_safetensors_model(weights))
    model = BagOfModels(models, bag.get("weights"), bag.get("segment"))
    model.eval()
    return model


class _Progress:
    """Stands in for the tqdm module inside demucs.apply: apply_model wraps its list of
    chunks in tqdm.tqdm(...) when progress=True; we report each chunk as PROGRESS."""

    def __init__(self, report) -> None:
        self.report = report

    def tqdm(self, iterable, **_kwargs):
        items = list(iterable)
        for i, item in enumerate(items):
            yield item
            self.report((i + 1) / max(1, len(items)))


def separate_demucs(audio: np.ndarray, sr: int, out_dir: Path, model_name: str,
                    progress=lambda fraction: None) -> None:
    import torch
    from demucs import apply as demucs_apply
    from demucs.apply import apply_model

    emit("STATUS", f"Loading model {model_name} from {MODELS_DIR}...")
    model = load_local_model(model_name)
    device = "mps" if torch.backends.mps.is_available() else ("cuda" if torch.cuda.is_available() else "cpu")

    if sr != model.samplerate:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(sr, model.samplerate)
        audio = resample_poly(audio, model.samplerate // g, sr // g, axis=0).astype(np.float32)
    wav = torch.from_numpy(audio.T.copy())
    ref = wav.mean(0)
    wav = (wav - ref.mean()) / (ref.std() + 1e-8)

    # apply_model shifts the song by a random amount (Demucs' "shift trick");
    # a fixed seed makes the same song always give the same stems.
    import random
    random.seed(0)
    torch.manual_seed(0)
    emit("STATUS", f"Separating on {device} (a few minutes)...")
    demucs_apply.tqdm = _Progress(progress)
    with torch.no_grad():
        sources = apply_model(model, wav[None], device=device, progress=True, split=True, overlap=0.25)[0]
    sources = sources * (ref.std() + 1e-8) + ref.mean()
    named = {DEMUCS_NAMES.get(theirs, theirs): tensor.cpu().numpy().T for theirs, tensor in zip(model.sources, sources)}
    # Keys are never charted: the piano stays inside the rest of the mix ("other").
    if "keys" in named and "other" in named:
        named["other"] = named["other"] + named.pop("keys")
    for name, audio in named.items():
        write_stem(out_dir, name, audio, model.samplerate)


def separate_simple(audio: np.ndarray, sr: int, out_dir: Path) -> None:
    """No AI: low-pass = bass, high-pass = the rest (for testing the window)."""
    from scipy.signal import butter, sosfiltfilt
    emit("STATUS", "Simple filter split (no AI)...")
    low = sosfiltfilt(butter(4, 250, btype="lowpass", fs=sr, output="sos"), audio, axis=0)
    write_stem(out_dir, "bass", low.astype(np.float32), sr)
    write_stem(out_dir, "other", (audio - low).astype(np.float32), sr)


def separate(song: Path, out_dir: Path, mode: str = "demucs", model: str = "htdemucs_6s",
             progress=lambda fraction: None) -> None:
    """Writes out_dir/<stem>.wav and out_dir/.done; progress(0..1) as it goes."""
    out_dir.mkdir(parents=True, exist_ok=True)
    emit("STATUS", f"Decoding {song.name}...")
    audio, sr = load_audio(song)
    write_stem(out_dir, "mix", audio, sr)
    progress(0.03)
    if mode == "demucs":
        separate_demucs(audio, sr, out_dir, model, lambda f: progress(0.03 + 0.95 * f))
    else:
        separate_simple(audio, sr, out_dir)
    (out_dir / ".done").touch()
    progress(1.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("song", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--mode", choices=("demucs", "simple"), default="demucs")
    parser.add_argument("--model", default="htdemucs_6s")
    args = parser.parse_args()
    try:
        separate(args.song, args.out_dir, args.mode, args.model,
                 lambda fraction: emit("PROGRESS", f"{fraction:.3f}"))
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        emit("ERROR", f"{type(exc).__name__}: {exc}".replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
