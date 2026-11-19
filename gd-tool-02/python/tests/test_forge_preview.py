"""forge.py's notes.mid -> preview.txt conversion: the seconds on the board must
be the seconds mido (and the game) read from the file."""

import sys
from pathlib import Path

import mido

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import forge  # noqa: E402


def make_midi(path: Path, first_tempo: int, numerator: int = 4) -> None:
    mid = mido.MidiFile(type=1, ticks_per_beat=192)
    sync = mido.MidiTrack()
    sync.append(mido.MetaMessage("track_name", name="song", time=0))
    sync.append(mido.MetaMessage("time_signature", numerator=numerator, denominator=4, time=0))
    sync.append(mido.MetaMessage("set_tempo", tempo=first_tempo, time=0))  # the song's first beat
    sync.append(mido.MetaMessage("set_tempo", tempo=480000, time=192))
    mid.tracks.append(sync)
    drums = mido.MidiTrack()
    drums.append(mido.MetaMessage("track_name", name="PART DRUMS", time=0))
    for _ in range(4):  # an Expert kick on every beat from beat 1
        drums.append(mido.Message("note_on", note=96, velocity=100, time=192 if len(drums) == 1 else 172))
        drums.append(mido.Message("note_off", note=96, velocity=0, time=20))
    mid.tracks.append(drums)
    mid.save(path)


def mido_seconds(path: Path) -> list[float]:
    t, out = 0.0, []
    for msg in mido.MidiFile(path):
        t += msg.time
        if msg.type == "note_on" and msg.velocity > 0:
            out.append(round(t, 4))
    return out


def preview_seconds(preview: Path, part: str = "drums", diff: str = "3") -> list[float]:
    return sorted(float(l.split()[3]) for l in preview.read_text().splitlines()
                  if l.startswith(f"NOTE {part} {diff} "))


def test_note_times_match_the_files_tempo_map(tmp_path):
    notes = tmp_path / "notes.mid"
    make_midi(notes, first_tempo=307181)  # not the MIDI default of 500000
    forge.write_preview(notes, tmp_path / "preview.txt", 10.0)
    assert preview_seconds(tmp_path / "preview.txt") == mido_seconds(notes)


def test_bars_follow_a_time_signature_set_at_tick_0(tmp_path):
    notes = tmp_path / "notes.mid"
    make_midi(notes, first_tempo=500000, numerator=3)
    forge.write_preview(notes, tmp_path / "preview.txt", 10.0)
    bars = [l.split()[2] for l in (tmp_path / "preview.txt").read_text().splitlines() if l.startswith("BEAT")]
    assert bars[:5] == ["1", "0", "0", "1", "0"]  # 3/4: a bar every 3 beats


def test_drum_steadiness_reaches_the_engine_with_the_genre(tmp_path):
    from ai_charter.config import load_rules

    rules = forge.write_rules(tmp_path, drum_similarity=0.8)
    loaded = load_rules(rules, "eletronica")
    assert loaded.drum_detection.repeat_similarity == 0.8
    assert loaded.general.min_stem_energy == 0.02  # the genre preset still applies


def test_riff_steadiness_reaches_guitar_bass_and_keys(tmp_path):
    from ai_charter.config import load_rules

    loaded = load_rules(forge.write_rules(tmp_path, drum_similarity=0.8, riff_similarity=0.8), "metal")
    assert [loaded.detection[p].repeat_similarity for p in ("guitar", "bass", "keys")] == [0.8, 0.8, 0.8]
    assert loaded.detection["guitar"].onset_delta == 0.03  # the genre preset still applies
