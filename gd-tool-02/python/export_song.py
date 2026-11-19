"""gd-tool-02 Export song: a project's chart + stems as a Clone Hero / YARG song folder.

    python export_song.py PROJECT_DIR --dest SONGS_DIR [--targets gh1,gh2,full]
                                      [--format clonehero|yarg]

One folder per target, in SONGS_DIR: "<Artist> - <Title> (<target>)" with notes.mid,
song.ini, album.png and one .ogg per stem. Targets follow the original setlists:

    gh1    Guitar Hero 1:             guitar                    guitar.ogg + song.ogg
    gh2    Guitar Hero 2 / 3 / Aerosmith: guitar and bass       guitar, bass, song
    full   Full band (GH4+ / Rock Band / YARG): guitar, bass, drums, vocals, one stem each

A part the target has but the chart lacks is left out (its audio goes to song.ogg).
Clone Hero and YARG read the same folder; --format only names the destination and the
loading phrase. Prints STATUS / PROGRESS / EXPORTED <folder> / DONE / ERROR lines.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import yaml

import chart_text
from midi_io import chart_to_midi

TARGETS = {
    "gh1": ("GH1", ["guitar"]),
    "gh2": ("GH2-GH3", ["guitar", "bass"]),
    "full": ("Full band", ["guitar", "bass", "drums", "vocals"]),
}
STEMS = ("drums", "bass", "other", "vocals", "guitar", "keys")
FORMATS = {"clonehero": "Clone Hero", "yarg": "YARG"}


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


def folder_name(text: str) -> str:
    text = re.sub(r'[/\\:*?"<>|\x00-\x1f]', "-", text).strip(" .")
    return text or "Untitled"


def difficulty(notes: list, duration: float) -> int:
    """0..6 from the Expert chords per second (Clone Hero's diff_ scale)."""
    if not notes or duration <= 0:
        return -1
    nps = len({round(n.t, 3) for n in notes if n.diff == 3}) / duration
    for level, limit in enumerate((1.0, 2.0, 3.0, 4.5, 6.0, 8.0)):
        if nps < limit:
            return level
    return 6


def load_stem(path: Path) -> tuple[np.ndarray, int]:
    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    return data[:, :2], sr


def offset_audio(data: np.ndarray, sr: int, ms: int) -> np.ndarray:
    """The song's audio against the charts (Song forge > Chart offset > Whole song audio):
    positive = the audio plays later (silence first), negative = earlier (its start is cut)."""
    frames = int(round(abs(ms) / 1000.0 * sr))
    if ms > 0:
        return np.concatenate([np.zeros((frames, data.shape[1]), dtype=data.dtype), data])
    return data[frames:] if ms < 0 else data


def write_ogg(path: Path, data: np.ndarray, sr: int) -> None:
    """Vorbis, in blocks: libsndfile (1.2) crashes when a whole song is written in one call."""
    data = np.clip(data, -1.0, 1.0).astype(np.float32)
    with sf.SoundFile(str(path), "w", sr, data.shape[1], format="OGG", subtype="VORBIS") as out:
        for start in range(0, len(data), 1 << 15):
            out.write(data[start:start + (1 << 15)])


def export_target(project: Path, doc: dict, chart: chart_text.Chart, base_midi: Path, target: str,
                  dest: Path, fmt: str, progress) -> Path:
    label, wanted = TARGETS[target]
    song = doc.get("song") or {}
    title, artist = str(song.get("title") or project.name), str(song.get("artist") or "Unknown Artist")
    out = dest / folder_name(f"{artist} - {title} ({label})")
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.ogg"):
        old.unlink()

    charted = set(chart.parts()) | ({"vocals"} if chart.vocals else set())
    parts = [p for p in wanted if p in charted]
    missing = [p for p in wanted if p not in charted]
    if not parts:
        raise ValueError(f"{label}: the chart has none of {', '.join(wanted)}")
    if missing:
        emit("STATUS", f"{label}: no {', '.join(missing)} chart, left out")

    # Audio: one stem per exported part, the rest mixed into song.ogg.
    forge = doc.get("forge") or {}
    audio_ms = int(float(forge.get("audio_offset_ms") or 0))
    stems_dir = project / "stems"
    source = {p: str(forge.get(f"{p}_source") or p) for p in ("guitar", "bass")}
    source.update({"drums": "drums", "vocals": "vocals"})
    used: dict[str, str] = {}
    for p in parts:
        stem = source[p]
        if (stems_dir / f"{stem}.wav").exists() and stem not in used.values():
            used[p] = stem
    rest = [s for s in STEMS if (stems_dir / f"{s}.wav").exists() and s not in used.values()]
    steps = len(used) + (1 if rest else 0) + 1
    done = 0
    for part, stem in used.items():
        emit("STATUS", f"{label}: {part}.ogg")
        data, sr = load_stem(stems_dir / f"{stem}.wav")
        write_ogg(out / f"{part}.ogg", offset_audio(data, sr, audio_ms), sr)
        done += 1
        progress(done / steps)
    if rest:
        emit("STATUS", f"{label}: song.ogg ({', '.join(rest)})")
        mix, sr = None, 44100
        for stem in rest:
            data, sr = load_stem(stems_dir / f"{stem}.wav")
            if mix is None:
                mix = data.copy()
            else:
                n = min(len(mix), len(data))
                mix = mix[:n] + data[:n]
        write_ogg(out / "song.ogg", offset_audio(mix, sr, audio_ms), sr)
        done += 1
        progress(done / steps)

    chart_to_midi(chart, base_midi, out / "notes.mid", [p for p in parts if p != "vocals"], "vocals" in parts)
    if (project / "cover.png").exists():
        shutil.copyfile(project / "cover.png", out / "album.png")
    write_song_ini(out / "song.ini", doc, chart, parts, label, fmt)
    progress(1.0)
    return out


def write_song_ini(path: Path, doc: dict, chart: chart_text.Chart, parts: list[str], label: str, fmt: str) -> None:
    song, project = doc.get("song") or {}, doc.get("project") or {}
    duration = chart.duration or max([n.t + n.sustain for n in chart.notes] + [0.0])
    diffs = {p: difficulty(chart.part(p), duration) if p in parts else -1 for p in ("guitar", "bass", "drums")}
    vocal_diff = -1
    if "vocals" in parts and chart.vocals and duration > 0:
        vocal_diff = min(6, int(len(chart.vocals) / duration * 2))
    band = max([d for d in diffs.values()] + [vocal_diff, 0])
    lines = {
        "name": song.get("title") or "Untitled",
        "artist": song.get("artist") or "Unknown Artist",
        "album": song.get("album") or "",
        "genre": song.get("genre") or "",
        "year": song.get("year") or "",
        "charter": project.get("charter") or "Dream Charter Engine",
        "song_length": int(round(duration * 1000)),
        "preview_start_time": int(round(duration * 300)),  # 30% in
        "delay": 0,
        "diff_band": band,
        "diff_guitar": diffs["guitar"],
        "diff_bass": diffs["bass"],
        "diff_drums": diffs["drums"],
        "diff_drums_real": diffs["drums"],
        "diff_vocals": vocal_diff,
        "pro_drums": "True" if "drums" in parts else "False",
        "five_lane_drums": "False",
        "loading_phrase": f"Forged with Dream Charter Engine ({label}, {FORMATS.get(fmt, fmt)})",
    }
    text = "[song]\n" + "".join(f"{k} = {v}\n" for k, v in lines.items())
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--dest", type=Path, required=True)
    parser.add_argument("--targets", default="full")
    parser.add_argument("--format", choices=tuple(FORMATS), default="clonehero")
    args = parser.parse_args()
    try:
        project = args.project
        doc = yaml.safe_load((project / "project.yaml").read_text(encoding="utf-8")) or {}
        chart_file = project / "forge" / "chart.txt"
        if not chart_file.exists():
            raise FileNotFoundError("no chart yet: forge the song first")
        base_midi = next((project / "forge").glob("*/notes.mid"), None)
        if base_midi is None:
            raise FileNotFoundError("the forge's notes.mid is missing: forge the song again")
        chart = chart_text.read(chart_file)
        targets = [t for t in args.targets.split(",") if t in TARGETS]
        if not targets:
            raise ValueError("no target chosen")
        for i, target in enumerate(targets):
            def progress(f: float, i: int = i) -> None:
                emit("PROGRESS", f"{(i + f) / len(targets):.3f}")
            out = export_target(project, doc, chart, base_midi, target, args.dest, args.format, progress)
            emit("EXPORTED", str(out))
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        emit("ERROR", f"{type(exc).__name__}: {exc}".replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
