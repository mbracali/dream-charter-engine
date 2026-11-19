"""gd-tool-02 Forge: charts a project with dream-charter-engine (ai_charter), from
the stems the project already has, then writes forge/preview.txt for the board.

    python forge.py PROJECT_DIR [--instruments guitar,bass,drums,keys,vocals]
                                [--sources guitar=other,keys=vocals] [--genre rock]
                                [--drum-similarity 0.8]

- the engine reads PROJECT_DIR/stems/ (no new separation) and writes
  PROJECT_DIR/forge/<Artist - Title>/notes.mid + song.ini;
- no lead-in silence (lead_in=0), so the chart's seconds are the stems' seconds;
- the game's .ogg files are not written here (that is Export's job), so ffmpeg is
  not needed: the engine's audio helpers are replaced by soundfile ones.

preview.txt, one event per line (seconds), read by the C++ board:
    SONG <duration>
    BEAT <t> <1 = first beat of a bar, else 0>
    SECTION <t> <name>
    NOTE <part> <difficulty 0-3> <t> <sustain> <lane> <flags>   flags: h HOPO, s Star Power,
                                                                  c cymbal, - none
    VOCAL <t> <duration> <pitch> <lyric>
    PHRASE <t> <end>
Prints STATUS / DONE / ERROR lines like the other helpers.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

# The engine's best parts, offline: Beat This! (beats and bar starts) reads its checkpoint
# from TORCH_HOME/hub/checkpoints/, kept in gd-tool-02/models/torch; ADTOF's weights ship
# inside its package. Nothing is downloaded at run time.
os.environ.setdefault("TORCH_HOME", str(Path(__file__).resolve().parent.parent / "models" / "torch"))
os.environ["HF_HUB_OFFLINE"] = "1"

import numpy as np
import soundfile as sf
import yaml

PARTS = {"PART GUITAR": "guitar", "PART BASS": "bass", "PART KEYS": "keys", "PART DRUMS": "drums"}
DIFF_BASE = {0: 60, 1: 72, 2: 84, 3: 96}  # Easy, Medium, Hard, Expert


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


# ---- the engine, without ffmpeg ------------------------------------------------

def patch_engine_audio() -> None:
    from ai_charter.export import audio

    def to_wav(src: Path, dst: Path, sample_rate: int = 44100) -> None:
        data, sr = sf.read(str(src), dtype="float32", always_2d=True)
        if sr != sample_rate:
            from math import gcd
            from scipy.signal import resample_poly
            g = gcd(sr, sample_rate)
            data = resample_poly(data, sample_rate // g, sr // g, axis=0).astype(np.float32)
        sf.write(str(dst), data[:, :2], sample_rate, subtype="PCM_16")

    audio.require_ffmpeg = lambda: None
    audio.read_tags = lambda src: {}
    audio.to_wav = to_wav
    audio.to_ogg = lambda *a, **k: None
    audio.mix_to_ogg = lambda *a, **k: None
    audio.extract_cover = lambda *a, **k: False


def write_rules(folder: Path, drum_similarity: float, riff_similarity: float = 0.0) -> Path:
    """Our rules on top of the engine's defaults and the genre preset (load_rules merges
    defaults -> genre -> this file). drum_similarity: bars at least this alike get the same
    drum notes (the engine's repeat_similarity; it ships 0.9, lower = steadier grooves).
    riff_similarity: the same for guitar, bass and keys (0 = off, the engine's default)."""
    path = folder / "forge_rules.toml"
    text = f"# gd-tool-02 Forge\n[drum_detection]\nrepeat_similarity = {drum_similarity:g}\n"
    for part in ("guitar", "bass", "keys"):
        text += f"\n[detection.{part}]\nrepeat_similarity = {riff_similarity:g}\n"
    path.write_text(text, encoding="utf-8")
    return path


def run_engine(project: Path, instruments: list[str], sources: dict[str, str], genre: str | None,
               drum_similarity: float = 0.9, riff_similarity: float = 0.0) -> Path:
    patch_engine_audio()
    from ai_charter.pipeline import Options, run

    doc = yaml.safe_load((project / "project.yaml").read_text(encoding="utf-8")) or {}
    song = project / str((doc.get("project") or {}).get("audio_file") or "")
    if not song.is_file():
        raise FileNotFoundError(f"the project has no song file ({song.name or 'audio_file is empty'})")
    stems = project / "stems"
    if not (stems / "drums.wav").exists() and not (stems / "guitar.wav").exists():
        raise FileNotFoundError("the project has no stems yet: separate it first")
    lyrics = project / "lyrics.txt"
    use_vocals = "vocals" in instruments and lyrics.exists() and lyrics.read_text(encoding="utf-8").strip()
    song_info = doc.get("song") or {}
    out = project / "forge"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()
    opts = Options(
        rules_path=write_rules(out, drum_similarity, riff_similarity),
        input=song, output_dir=out, separator="folder", stems_dir=stems,
        genre=genre or None, lead_in=0.0,
        instruments=tuple(i for i in instruments if i != "vocals"),
        stem_sources=sources or None,
        title=str(song_info.get("title") or project.name),
        artist=str(song_info.get("artist") or "Unknown Artist"),
        lyrics=lyrics if use_vocals else None,
    )
    result = run(opts, log=lambda message: emit("STATUS", str(message).strip()))
    return result.song_dir / "notes.mid"


# ---- notes.mid -> preview.txt ----------------------------------------------------

class TempoMap:
    def __init__(self, midi) -> None:
        self.res = midi.ticks_per_beat
        self.changes: list[tuple[int, int]] = []  # (tick, microseconds per beat)
        self.meters: list[tuple[int, int]] = []   # (tick, beats per bar)
        for msg_tick, msg in absolute(midi.tracks[0]):
            if msg.type == "set_tempo":
                self.changes.append((msg_tick, msg.tempo))
            elif msg.type == "time_signature":
                self.meters.append((msg_tick, msg.numerator))
        # By tick only (stable): a plain sort would order two events at the same tick by
        # their value. The MIDI defaults (120 BPM, 4/4) only count when the file sets
        # nothing at tick 0.
        self.changes.sort(key=lambda c: c[0])
        self.meters.sort(key=lambda m: m[0])
        if not self.changes or self.changes[0][0] != 0:
            self.changes.insert(0, (0, 500000))
        if not self.meters or self.meters[0][0] != 0:
            self.meters.insert(0, (0, 4))

    def seconds(self, tick: int) -> float:
        t, last_tick, tempo = 0.0, 0, self.changes[0][1]
        for change_tick, change_tempo in self.changes:
            if change_tick > tick:
                break
            t += (change_tick - last_tick) * tempo / 1e6 / self.res
            last_tick, tempo = change_tick, change_tempo
        return t + (tick - last_tick) * tempo / 1e6 / self.res


def absolute(track):
    tick = 0
    for msg in track:
        tick += msg.time
        yield tick, msg


def spans(track, pitches: set[int]) -> list[tuple[int, int, int]]:
    """(start, end, pitch) for the note-on/off pairs of the given pitches."""
    open_notes: dict[int, int] = {}
    out = []
    for tick, msg in absolute(track):
        if msg.type not in ("note_on", "note_off") or msg.note not in pitches:
            continue
        if msg.type == "note_on" and msg.velocity > 0:
            open_notes[msg.note] = tick
        elif msg.note in open_notes:
            out.append((open_notes.pop(msg.note), tick, msg.note))
    return out


def inside(tick: int, ranges: list[tuple[int, int, int]]) -> bool:
    return any(a <= tick < b for a, b, _ in ranges)


def write_preview(notes_mid: Path, out: Path, duration: float) -> dict[str, list[int]]:
    import mido

    midi = mido.MidiFile(str(notes_mid))
    tempo = TempoMap(midi)
    res = tempo.res
    lines = [f"SONG {duration:.3f}"]
    end_tick = max(sum(m.time for m in tr) for tr in midi.tracks)

    # Beats and bars from the tempo map and the time signatures.
    tick, beat_in_bar, meter_i = 0, 0, 0
    while tick <= end_tick:
        while meter_i + 1 < len(tempo.meters) and tempo.meters[meter_i + 1][0] <= tick:
            meter_i += 1
            beat_in_bar = 0
        lines.append(f"BEAT {tempo.seconds(tick):.4f} {1 if beat_in_bar == 0 else 0}")
        beat_in_bar = (beat_in_bar + 1) % max(1, tempo.meters[meter_i][1])
        tick += res

    counts: dict[str, list[int]] = {}
    for track in midi.tracks:
        name = track.name.strip()
        if name == "EVENTS":
            for t, msg in absolute(track):
                if msg.type == "text" and msg.text.startswith("[section"):
                    lines.append(f"SECTION {tempo.seconds(t):.4f} {msg.text[9:].rstrip(']').strip()}")
        elif name in PARTS:
            part = PARTS[name]
            drums = part == "drums"
            star = spans(track, {116})
            toms = {lane: spans(track, {108 + lane}) for lane in (2, 3, 4)}  # 110-112: yellow/blue/green toms
            counts[part] = [0, 0, 0, 0]
            for diff, base in DIFF_BASE.items():
                forced_hopo = spans(track, {base + 5})
                forced_strum = spans(track, {base + 6})
                notes = sorted(spans(track, set(range(base, base + 5))))
                by_tick: dict[int, list[tuple[int, int]]] = {}
                for a, b, p in notes:
                    by_tick.setdefault(a, []).append((b, p - base))
                prev_tick, prev_lanes = None, None
                for a in sorted(by_tick):
                    group = by_tick[a]
                    lanes = sorted(l for _, l in group)
                    hopo = False
                    if not drums:
                        natural = (prev_tick is not None and a - prev_tick <= res // 3 and len(lanes) == 1
                                   and lanes != prev_lanes)
                        hopo = (natural or inside(a, forced_hopo)) and not inside(a, forced_strum)
                    for b, lane in group:
                        flags = ""
                        if hopo:
                            flags += "h"
                        if inside(a, star):
                            flags += "s"
                        if drums and lane >= 2 and not inside(a, toms[lane]):
                            flags += "c"
                        start = tempo.seconds(a)
                        # Short notes are taps; only notes longer than ~1/3 beat keep a sustain.
                        sustain = tempo.seconds(b) - start if (b - a) > res // 3 and not drums else 0.0
                        lines.append(f"NOTE {part} {diff} {start:.4f} {sustain:.4f} {lane} {flags or '-'}")
                    counts[part][diff] += 1  # a chord (or kick + cymbal) counts once, like the engine
                    prev_tick, prev_lanes = a, lanes
        elif name == "PART VOCALS":
            lyric_at: dict[int, str] = {}
            for t, msg in absolute(track):
                if msg.type in ("lyrics", "text") and not msg.text.startswith("["):
                    lyric_at[t] = msg.text
            for a, b, p in spans(track, set(range(36, 85))):
                ta = tempo.seconds(a)
                lines.append(f"VOCAL {ta:.4f} {tempo.seconds(b) - ta:.4f} {p} {lyric_at.get(a, '')}")
            for a, b, _ in spans(track, {105, 106}):
                lines.append(f"PHRASE {tempo.seconds(a):.4f} {tempo.seconds(b):.4f}")
            counts["vocals"] = [sum(1 for l in lines if l.startswith("VOCAL "))] * 4
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--instruments", default="guitar,bass,drums,keys")
    parser.add_argument("--sources", default="")
    parser.add_argument("--genre", default="")
    parser.add_argument("--drum-similarity", type=float, default=0.8,
                        help="0.9 = the engine's default (as heard), 0.8 steady, 0.7 locked")
    parser.add_argument("--riff-similarity", type=float, default=0.8,
                        help="the same for guitar, bass and keys; 0 = off (the engine's default)")
    parser.add_argument("--preview-only", type=Path, help="debug: convert this notes.mid only")
    args = parser.parse_args()
    try:
        instruments = [i for i in args.instruments.split(",") if i]
        sources = dict(p.split("=", 1) for p in args.sources.split(",") if "=" in p)
        if args.preview_only:
            notes = args.preview_only
        else:
            if not instruments:
                raise ValueError("no part selected")
            notes = run_engine(args.project, instruments, sources, args.genre, args.drum_similarity,
                               args.riff_similarity)
        emit("STATUS", "Preparing the board...")
        duration = 0.0
        stems = args.project / "stems"
        for wav in stems.glob("*.wav"):
            duration = max(duration, sf.info(str(wav)).duration)
        counts = write_preview(notes, args.project / "forge" / "preview.txt", duration)
        for part, c in counts.items():
            emit("COUNT", part, *map(str, c))
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        emit("ERROR", f"{type(exc).__name__}: {exc}".replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
