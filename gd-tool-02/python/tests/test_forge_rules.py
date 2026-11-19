"""R18-R20: guitar / bass come from their sheet only, need a sheet (or "no guitar"), and the
window hears English."""

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import forge  # noqa: E402

TAB = "\n".join(f"{s}|--0--2--3--|" for s in "eBGDAE") + "\n"


class FakeGuide:
    def __init__(self, applied, span=None):
        self.instrument, self.span, self.applied = "guitar", span, applied
        self.stats = {}

    def __call__(self, cands, source="", log=print):
        self.stats = {"applied": self.applied}
        return cands


def notes(*ticks):
    return [SimpleNamespace(tick=t) for t in ticks]


def test_a_sheet_that_does_not_match_charts_nothing():
    assert forge.ScoreOnly(FakeGuide(applied=False))(notes(0, 100, 200)) == []


def test_only_the_part_the_sheet_covers_is_charted():
    out = forge.ScoreOnly(FakeGuide(applied=True, span=(100, 300)))(notes(0, 100, 200, 300, 400))
    assert [c.tick for c in out] == [100, 200, 300]


def project(tmp_path, sheets_doc=None, files=()):
    for box, name, text in files:
        (tmp_path / "sheets" / box).mkdir(parents=True, exist_ok=True)
        (tmp_path / "sheets" / box / name).write_text(text)
    (tmp_path / "forge").mkdir(exist_ok=True)
    return {"sheets": sheets_doc or {}}


def test_guitar_and_bass_need_a_sheet(tmp_path):
    doc = project(tmp_path, files=[("guitar", "riff.txt", TAB)])
    with pytest.raises(ValueError, match="bass needs a sheet"):
        forge.Plan(tmp_path, doc)


def test_no_bass_in_the_song_skips_it(tmp_path):
    doc = project(tmp_path, {"bass_absent": "yes"}, [("guitar", "riff.txt", TAB)])
    plan = forge.Plan(tmp_path, doc)
    assert plan.skipped == ["bass"] and "bass" not in plan.engine_parts
    assert plan.fidelity["guitar"] == "seguir" and plan.refs["guitar"].suffix == ".mid"
    assert plan.riff_by_part["guitar"] == 0.0  # a sheet-driven part is not "steadied"


def test_a_chord_sheet_is_enough(tmp_path):
    doc = project(tmp_path, files=[("guitar", "chords.txt", "Em  C  D\nwords\n"), ("bass", "riff.txt", TAB)])
    plan = forge.Plan(tmp_path, doc)
    assert "guitar" in plan.engine_parts and "guitar" not in plan.refs and plan.chords.name == "chords.txt"


def test_the_window_gets_english():
    assert forge.translate("2/5 Detectando o andamento (BPM)...") == "Step 2 of 5: finding the beat and the bars..."
    assert forge.translate("    guitar: referência confere com 64% das notas detectadas; 826 corrigidas") == \
        "guitar: the sheet matches 64% of what is heard"
    assert forge.translate("    drums: a referência confere com só 12% das batidas, ignorada") == \
        "drums: the sheet matches only 12% of what is heard"
    assert forge.translate("    afinação da gravação: +3 centésimos") is None  # log file only
    assert forge.english_error(RuntimeError("a partitura não tem pistas")).startswith("the engine stopped")
    assert forge.english_error(ValueError("the bass needs a sheet")) == "the bass needs a sheet"


def test_no_drums_in_the_song_skips_them_and_keys_never_chart(tmp_path):
    doc = project(tmp_path, {"drums_absent": "yes", "bass_absent": "yes"}, [("guitar", "riff.txt", TAB)])
    doc["forge"] = {"keys": "on"}  # an old project's keys option is ignored (R25)
    plan = forge.Plan(tmp_path, doc)
    assert plan.skipped == ["bass", "drums"]
    assert plan.parts == ["guitar"] and plan.engine_parts == ["guitar"]


def test_a_charts_keys_track_is_ignored(tmp_path):
    import mido

    from midi_io import midi_parts, midi_to_chart

    mid = mido.MidiFile(type=1, ticks_per_beat=480)
    mid.tracks.append(mido.MidiTrack([mido.MetaMessage("set_tempo", tempo=500000, time=0)]))
    for name in ("PART KEYS", "PART GUITAR"):
        track = mido.MidiTrack([mido.MetaMessage("track_name", name=name, time=0)])
        track += [mido.Message("note_on", note=96, velocity=100, time=0), mido.Message("note_off", note=96, velocity=0, time=240)]
        mid.tracks.append(track)
    mid.save(tmp_path / "notes.mid")
    assert midi_parts(tmp_path / "notes.mid") == {"guitar"}
    assert midi_to_chart(tmp_path / "notes.mid").parts() == ["guitar"]
