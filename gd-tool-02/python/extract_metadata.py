"""gd-tool-02: everything that can be read from a song file by code, into the
project's project.yaml (plus cover.png and lyrics.txt when the file has them).

    python extract_metadata.py SONG PROJECT_DIR

- tags (mutagen): title, artist, album, album artist, year, genre, track, disc,
  composer, language, comment, embedded lyrics and cover art;
- no tags: "Artist - Title" from the file name;
- the file: format, codec, duration, sample rate, channels, bitrate, size, SHA-256;
- the audio (librosa, offline): BPM, musical key (+ confidence), leading
  silence, peak and RMS level.

Values the user already filled in project.yaml are kept: only empty fields are
filled. Prints STATUS / DONE / ERROR lines like separate_stems.py.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import io
import json
import re
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import yaml

# The project file's layout; the C++ app (src/project.cpp) uses the same one.
SCHEMA: dict[str, list[str]] = {
    "project": ["name", "created", "source_file", "audio_file", "charter", "notes"],
    "song": ["title", "artist", "album", "album_artist", "year", "genre", "track", "disc",
             "composer", "language", "comment"],
    "analysis": ["bpm", "key", "key_confidence", "start_silence_s", "peak_dbfs", "rms_dbfs"],
    "audio": ["format", "codec", "duration_s", "sample_rate", "channels", "bitrate_kbps",
              "file_size_mb", "sha256"],
    "stems": ["mode", "model", "status", "error", "separated_at"],
}
HEADER = ("# gd-tool-02 project file. Edit it in the app (Metadata) or by hand,\n"
          "# keeping the layout: a section, then indented `key: value` lines.\n")


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


# ---- project.yaml -------------------------------------------------------------

def scalar(value) -> str:
    if value is None:
        return '""'
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    if isinstance(value, (float, np.floating)):
        return f"{float(value):.6g}"
    return json.dumps(str(value), ensure_ascii=False)  # a JSON string is a valid YAML string


def write_project(path: Path, doc: dict) -> None:
    lines = [HEADER]
    sections = list(SCHEMA) + [s for s in doc if s not in SCHEMA]
    for section in sections:
        values = doc.get(section) or {}
        keys = SCHEMA.get(section, []) + [k for k in values if k not in SCHEMA.get(section, [])]
        lines.append(f"{section}:")
        lines += [f"  {key}: {scalar(values.get(key))}" for key in keys]
        lines.append("")
    tmp = path.with_suffix(".yaml.tmp")
    tmp.write_text("\n".join(lines), encoding="utf-8")
    tmp.replace(path)


def empty(value) -> bool:
    return value is None or (isinstance(value, str) and not value.strip())


def fill(doc: dict, section: str, values: dict) -> None:
    target = doc.setdefault(section, {}) or {}
    doc[section] = target
    for key, value in values.items():
        if not empty(value) and empty(target.get(key)):
            target[key] = value


# ---- tags ---------------------------------------------------------------------

def first(tags, *keys):
    for key in keys:
        try:
            value = tags.get(key)
        except (KeyError, ValueError):
            continue
        if value:
            value = value[0] if isinstance(value, list) else value
            text = str(value).strip()
            if text:
                return text
    return None


def read_tags(path: Path) -> tuple[dict, str | None, bytes | None, object]:
    import mutagen

    song: dict = {}
    lyrics = cover = None
    easy = mutagen.File(str(path), easy=True)
    raw = mutagen.File(str(path))
    if easy is not None and easy.tags is not None:
        t = easy.tags
        song = {
            "title": first(t, "title"),
            "artist": first(t, "artist"),
            "album": first(t, "album"),
            "album_artist": first(t, "albumartist", "album artist"),
            "year": (first(t, "date", "originaldate", "year") or "")[:4] or None,
            "genre": first(t, "genre"),
            "track": (first(t, "tracknumber") or "").split("/")[0] or None,
            "disc": (first(t, "discnumber") or "").split("/")[0] or None,
            "composer": first(t, "composer"),
            "language": first(t, "language"),
        }
    if raw is not None and raw.tags is not None:
        tags = raw.tags
        keys = list(tags.keys())
        # ID3 (MP3): USLT lyrics, APIC cover, COMM comment.
        for key in keys:
            frame = tags[key]
            if key.startswith("USLT") and not lyrics:
                lyrics = str(frame.text)
            elif key.startswith("APIC") and cover is None:
                cover = frame.data
            elif key.startswith("COMM") and not song.get("comment"):
                text = " ".join(str(x) for x in getattr(frame, "text", []))
                if text.strip():
                    song["comment"] = text.strip()
        # Vorbis comments (FLAC/OGG) and MP4.
        if not lyrics:
            lyrics = first(tags, "lyrics", "LYRICS", "unsyncedlyrics", "UNSYNCEDLYRICS", "\xa9lyr")
        if not song.get("comment"):
            song["comment"] = first(tags, "comment", "COMMENT", "description", "\xa9cmt")
        if cover is None and "covr" in keys and tags["covr"]:
            cover = bytes(tags["covr"][0])
    pictures = getattr(raw, "pictures", None)  # FLAC
    if cover is None and pictures:
        cover = pictures[0].data
    return song, lyrics, cover, raw


def from_filename(path: Path) -> dict:
    """ "01 - Artist - Title" / "Artist - Title" / "Title" """
    stem = re.sub(r"^\d+\s*[-.]\s*", "", path.stem).replace("_", " ").strip()
    parts = [p.strip() for p in stem.split(" - ", 1)]
    if len(parts) == 2 and all(parts):
        return {"artist": parts[0], "title": parts[1]}
    return {"title": stem}


# ---- the audio ------------------------------------------------------------------

KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# Krumhansl-Kessler key profiles.
MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])


def detect_key(chroma_mean: np.ndarray) -> tuple[str, float]:
    scores = []
    for tonic in range(12):
        for name, profile in (("major", MAJOR), ("minor", MINOR)):
            r = np.corrcoef(chroma_mean, np.roll(profile, tonic))[0, 1]
            scores.append((r, f"{KEYS[tonic]} {name}"))
    scores.sort(reverse=True)
    best, second = scores[0], scores[1]
    confidence = max(0.0, min(1.0, (best[0] - second[0]) * 5 + best[0] * 0.5))
    return best[1], round(confidence, 2)


def analyse(path: Path) -> dict:
    import librosa

    full, sr_full = sf.read(str(path), dtype="float32", always_2d=True)
    peak = float(np.max(np.abs(full))) if full.size else 0.0
    rms = float(np.sqrt(np.mean(full ** 2))) if full.size else 0.0
    mono = full.mean(axis=1)
    loud = np.flatnonzero(np.abs(mono) > 10 ** (-50 / 20))  # first sound above -50 dBFS
    start = float(loud[0]) / sr_full if loud.size else 0.0

    sr = 22050
    y = librosa.resample(mono, orig_sr=sr_full, target_sr=sr) if sr_full != sr else mono
    emit("PROGRESS", "0.45")
    emit("STATUS", "Detecting the tempo...")
    tempo = librosa.feature.tempo(y=y, sr=sr, aggregate=np.median)
    emit("PROGRESS", "0.65")
    emit("STATUS", "Detecting the key...")
    harmonic = librosa.effects.harmonic(y)
    chroma = librosa.feature.chroma_cqt(y=harmonic, sr=sr).mean(axis=1)
    key, confidence = detect_key(chroma)
    return {
        "bpm": round(float(np.atleast_1d(tempo)[0]), 1),
        "key": key,
        "key_confidence": confidence,
        "start_silence_s": round(start, 3),
        "peak_dbfs": round(20 * np.log10(peak), 2) if peak > 0 else -120.0,
        "rms_dbfs": round(20 * np.log10(rms), 2) if rms > 0 else -120.0,
    }


def file_info(path: Path, raw) -> dict:
    info = sf.info(str(path))
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    bitrate = getattr(getattr(raw, "info", None), "bitrate", None)
    size = path.stat().st_size
    if not bitrate and info.duration:
        bitrate = size * 8 / info.duration
    return {
        "format": info.format,
        "codec": info.subtype_info or info.subtype,
        "duration_s": round(info.duration, 3),
        "sample_rate": info.samplerate,
        "channels": info.channels,
        "bitrate_kbps": int(round(bitrate / 1000)) if bitrate else None,
        "file_size_mb": round(size / 1e6, 2),
        "sha256": digest.hexdigest(),
    }


def save_cover(data: bytes, out: Path) -> bool:
    from PIL import Image
    try:
        image = Image.open(io.BytesIO(data)).convert("RGB")
    except Exception:  # noqa: BLE001 - a broken picture is just no cover
        return False
    image.thumbnail((600, 600))
    image.save(out, "PNG")
    image.save(out.with_suffix(".bmp"), "BMP")  # for the window: SDL reads BMP everywhere (PNG needs SDL 3.4)
    return True


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    song_path, project_dir = Path(sys.argv[1]), Path(sys.argv[2])
    project_file = project_dir / "project.yaml"
    try:
        doc = yaml.safe_load(project_file.read_text(encoding="utf-8")) if project_file.exists() else {}
        doc = doc or {}
        emit("PROGRESS", "0.05")
        emit("STATUS", "Reading the tags...")
        song, lyrics, cover, raw = read_tags(song_path)
        fill(doc, "song", song)
        fill(doc, "song", from_filename(song_path))
        title, artist = doc["song"].get("title"), doc["song"].get("artist")
        fill(doc, "project", {"name": f"{artist} - {title}" if artist and title else title})
        fill(doc, "audio", file_info(song_path, raw))
        emit("PROGRESS", "0.2")
        emit("STATUS", "Analysing the audio...")
        fill(doc, "analysis", analyse(song_path))
        if cover and not (project_dir / "cover.png").exists() and save_cover(cover, project_dir / "cover.png"):
            emit("STATUS", "Cover art saved.")
        if lyrics and lyrics.strip() and not (project_dir / "lyrics.txt").exists():
            (project_dir / "lyrics.txt").write_text(lyrics.strip() + "\n", encoding="utf-8")
        fill(doc, "project", {"metadata_extracted_at": dt.datetime.now().isoformat(timespec="seconds")})
        write_project(project_file, doc)
        emit("PROGRESS", "1")
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        emit("ERROR", f"{type(exc).__name__}: {exc}".replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
