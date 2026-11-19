"""chart.txt <-> notes.mid, the sheets (Song home) and Export song."""

import sys
from pathlib import Path

import mido
import numpy as np
import soundfile as sf
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import chart_text  # noqa: E402
import export_song  # noqa: E402
import sheets  # noqa: E402
from chart_text import Chart, Note, Vocal  # noqa: E402
from midi_io import chart_to_midi, midi_parts, midi_to_chart  # noqa: E402


def base_midi(path: Path, tempo: int = 400000) -> Path:
    """A tempo map only (what the engine's notes.mid gives Export)."""
    mid = mido.MidiFile(type=1, ticks_per_beat=480)
    sync = mido.MidiTrack()
    sync.append(mido.MetaMessage("track_name", name="song", time=0))
    sync.append(mido.MetaMessage("time_signature", numerator=4, denominator=4, time=0))
    sync.append(mido.MetaMessage("set_tempo", tempo=tempo, time=0))
    sync.append(mido.MetaMessage("set_tempo", tempo=tempo // 2, time=480 * 8))  # faster after 2 bars
    mid.tracks.append(sync)
    mid.save(path)
    return path


def sample_chart() -> Chart:
    beat = 0.4
    notes = [
        Note("guitar", 3, 0.0, 0.0, 0, ""),           # strum
        Note("guitar", 3, beat / 4, 0.0, 1, "h"),     # natural HOPO (close, different lane)
        Note("guitar", 3, beat / 2, 0.0, 1, "h"),     # forced HOPO (same lane: not natural)
        Note("guitar", 3, beat * 0.75, 0.0, 2, ""),   # forced strum (would be a natural HOPO)
        Note("guitar", 3, beat * 2, 0.8, 0, "s"),     # chord with sustain, star power
        Note("guitar", 3, beat * 2, 0.8, 2, "s"),
        Note("guitar", 0, beat * 2, 0.0, 0, "s"),     # MIDI star power covers every difficulty
        Note("bass", 3, beat, 0.0, 3, ""),
        Note("drums", 3, 0.0, 0.0, 0, ""),
        Note("drums", 3, 0.0, 0.0, 2, "c"),           # yellow cymbal
        Note("drums", 3, beat, 0.0, 3, ""),           # blue tom
        Note("drums", 3, 4.0, 0.0, 1, ""),            # after the tempo change
    ]
    return Chart(duration=6.0, notes=notes, sections=[(0.0, "Intro"), (3.2, "Verse 1")],
                 vocals=[Vocal(0.4, 0.3, 60, "Hel-"), Vocal(0.8, 0.5, 64, "lo")], phrases=[(0.4, 1.3)])


def same_notes(a: list[Note], b: list[Note]) -> bool:
    key = lambda n: (n.part, n.diff, round(n.t, 2), n.lane)  # noqa: E731
    if sorted(map(key, a)) != sorted(map(key, b)):
        return False
    by = {key(n): n for n in b}
    return all(abs(by[key(n)].sustain - n.sustain) < 0.02 and set(by[key(n)].flags) == set(n.flags) for n in a)


def test_chart_text_round_trip(tmp_path):
    chart = sample_chart()
    chart_text.write(chart, tmp_path / "chart.txt")
    back = chart_text.read(tmp_path / "chart.txt")
    assert same_notes(chart.notes, back.notes)
    assert [v.lyric for v in back.vocals] == ["Hel-", "lo"]
    assert back.sections[1] == (3.2, "Verse 1")


def test_chart_to_midi_and_back_keeps_every_flag(tmp_path):
    chart = sample_chart()
    chart_to_midi(chart, base_midi(tmp_path / "base.mid"), tmp_path / "notes.mid", ["guitar", "bass", "drums"], True)
    back = midi_to_chart(tmp_path / "notes.mid", chart.duration)
    assert same_notes(chart.notes, back.notes)
    assert [(round(v.t, 3), v.pitch, v.lyric) for v in back.vocals] == [(0.4, 60, "Hel-"), (0.8, 64, "lo")]
    assert [name for _, name in back.sections] == ["Intro", "Verse 1"]
    assert midi_parts(tmp_path / "notes.mid") == {"guitar", "bass", "drums", "vocals"}


def test_kinds_by_extension_and_content(tmp_path):
    chart = sample_chart()
    chart_to_midi(chart, base_midi(tmp_path / "base.mid"), tmp_path / "chart.mid", ["guitar"], False)
    assert sheets.kind_of(tmp_path / "chart.mid", "guitar") == "chart"
    assert sheets.kind_of(tmp_path / "base.mid", "guitar") == "score"  # no PART tracks
    assert sheets.kind_of(Path("song.gp5"), "bass") == "tab"
    (tmp_path / "cifra.txt").write_text("Em  C  D\nwords to sing\n")
    assert sheets.kind_of(tmp_path / "cifra.txt", "guitar") == "chords"
    assert sheets.kind_of(tmp_path / "cifra.txt", "lyrics") == "lyrics"
    assert sheets.kind_of(Path("photo.png"), "drums") is None


def test_checked_files_follow_the_use_list(tmp_path):
    box = tmp_path / "sheets" / "guitar"
    box.mkdir(parents=True)
    for name in ("a.gp5", "b.txt", "c.png"):
        (box / name).write_text("x")
    assert [f.name for f in sheets.checked(tmp_path, {}, "guitar")] == ["a.gp5", "b.txt"]
    doc = {"sheets": {"guitar_use": "b.txt"}}
    assert [f.name for f in sheets.checked(tmp_path, doc, "guitar")] == ["b.txt"]
    assert sheets.checked(tmp_path, {"sheets": {"guitar_use": "none"}}, "guitar") == []


def test_lrc_lyrics_lose_their_timestamps(tmp_path):
    lrc = tmp_path / "song.lrc"
    lrc.write_text("[ar: Someone]\n[00:12.30]Hello <00:12.80>world\n[00:15.00]Second line\n", encoding="utf-8")
    assert sheets.lyrics_text(lrc) == "Hello world\nSecond line"


def test_dot_chart_parts_hopos_and_cymbals(tmp_path):
    text = """[Song]
{
  Resolution = 192
}
[SyncTrack]
{
  0 = TS 4
  0 = B 120000
}
[Events]
{
  0 = E "section Intro"
}
[ExpertSingle]
{
  0 = N 0 0
  64 = N 1 0
  128 = N 1 0
  128 = N 5 0
  384 = N 2 192
  384 = S 2 200
}
[ExpertDrums]
{
  0 = N 0 0
  0 = N 2 0
  0 = N 66 0
  96 = N 3 0
}
"""
    path = tmp_path / "notes.chart"
    path.write_text(text, encoding="utf-8")
    guitar = sorted(sheets.chart_part(path, "guitar"), key=lambda n: n.t)
    assert [(round(n.t, 3), n.lane, n.hopo) for n in guitar] == [(0.0, 0, False), (0.167, 1, True), (0.333, 1, True),
                                                                  (1.0, 2, False)]
    assert guitar[-1].star and abs(guitar[-1].sustain - 0.5) < 1e-6
    drums = sorted(sheets.chart_part(path, "drums"), key=lambda n: (n.t, n.lane))
    assert [(n.lane, n.cymbal) for n in drums] == [(0, False), (2, True), (3, False)]


def make_project(tmp_path: Path) -> Path:
    project = tmp_path / "Song"
    (project / "stems").mkdir(parents=True)
    (project / "forge" / "Artist - Song").mkdir(parents=True)
    sr = 22050
    t = np.arange(sr * 2) / sr
    for i, stem in enumerate(("drums", "bass", "other", "vocals", "guitar", "keys")):
        sf.write(str(project / "stems" / f"{stem}.wav"), 0.1 * np.sin(2 * np.pi * (110 + 50 * i) * t), sr)
    (project / "project.yaml").write_text(yaml.safe_dump({"song": {"title": "Song", "artist": "Artist"}}))
    base_midi(project / "forge" / "Artist - Song" / "notes.mid")
    chart = sample_chart()
    chart.duration = 2.0
    chart_text.write(chart, project / "forge" / "chart.txt")
    return project


def test_export_targets_follow_the_setlists(tmp_path):
    project = make_project(tmp_path)
    doc = yaml.safe_load((project / "project.yaml").read_text())
    chart = chart_text.read(project / "forge" / "chart.txt")
    base = project / "forge" / "Artist - Song" / "notes.mid"
    dest = tmp_path / "Songs"
    files = {}
    for target in ("gh1", "gh2", "full"):
        out = export_song.export_target(project, doc, chart, base, target, dest, "clonehero", lambda f: None)
        files[target] = sorted(p.name for p in out.iterdir())
    assert files["gh1"] == ["guitar.ogg", "notes.mid", "song.ini", "song.ogg"]
    assert files["gh2"] == ["bass.ogg", "guitar.ogg", "notes.mid", "song.ini", "song.ogg"]
    assert files["full"] == ["bass.ogg", "drums.ogg", "guitar.ogg", "notes.mid", "song.ini", "song.ogg", "vocals.ogg"]
    assert midi_parts(dest / "Artist - Song (GH1)" / "notes.mid") == {"guitar"}
    assert midi_parts(dest / "Artist - Song (Full band)" / "notes.mid") == {"guitar", "bass", "drums", "vocals"}
    ini = (dest / "Artist - Song (GH1)" / "song.ini").read_text()
    assert "diff_bass = -1" in ini and "name = Song" in ini
    assert sf.info(str(dest / "Artist - Song (GH1)" / "song.ogg")).format == "OGG"


def test_syllables_outside_every_phrase_get_one():
    from midi_io import vocal_phrases

    chart = Chart(vocals=[Vocal(1.0, 0.3, 60, "in"), Vocal(1.45, 0.2, 62, "late"), Vocal(5.0, 0.4, 64, "far")],
                  phrases=[(0.9, 1.4)])
    phrases = vocal_phrases(chart)
    for v in chart.vocals:
        assert any(a - 1e-3 <= v.t < b for a, b in phrases), v
    assert phrases[0][0] == 0.9 and len(phrases) == 2


def test_a_whole_song_encodes_to_ogg(tmp_path):
    """libsndfile crashes when a long song goes to Vorbis in one write: write_ogg chunks it."""
    sr = 44100
    data = (0.2 * np.sin(2 * np.pi * 220 * np.arange(sr * 240) / sr)).astype(np.float32)
    export_song.write_ogg(tmp_path / "long.ogg", np.stack([data, data], axis=1), sr)
    info = sf.info(str(tmp_path / "long.ogg"))
    assert info.format == "OGG" and abs(info.duration - 240) < 0.1
