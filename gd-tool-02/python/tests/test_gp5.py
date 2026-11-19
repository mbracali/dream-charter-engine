"""Guitar Pro 4 and 5 sheets (R31, R37): a real .gp4 / .gp5 written with pyguitarpro is read by
the engine, each instrument finds its own track, and the same file in several boxes is lined
up once. Every test runs for both formats."""

import shutil
import sys
from pathlib import Path

import pytest

pytest.importorskip("guitarpro")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import forge  # noqa: E402
import sheets  # noqa: E402
from gp5_fixture import write_gp5  # noqa: E402

# An E5 power chord, then a riff on the A string; bass roots; a kick / snare groove.
GUITAR = [(0.0, 40), (0.0, 47), (0.0, 52), (1.0, 45), (1.5, 47), (2.0, 48), (2.5, 50), (3.0, 52)]
BASS = [(0.0, 28), (1.0, 33), (2.0, 31), (3.0, 33)]
DRUMS = [(0.0, 36), (1.0, 38), (2.0, 36), (3.0, 38), (0.0, 42), (1.0, 42), (2.0, 42), (3.0, 42)]


@pytest.fixture(params=["gp5", "gp4"])
def fmt(request):
    return request.param


@pytest.fixture
def gp5(tmp_path, fmt):
    return write_gp5(tmp_path / f"song.{fmt}", GUITAR, 140, bass=BASS, drums=DRUMS)


def test_the_engine_reads_every_track(gp5):
    from ai_charter.reference import read_score

    tracks = {t.guess: t for t in read_score(gp5).tracks if t.notes}
    assert set(tracks) == {"guitar", "bass", "drums"}
    assert sorted((n.beat, n.pitch) for n in tracks["guitar"].notes) == sorted(GUITAR)
    assert sorted((n.beat, n.pitch) for n in tracks["bass"].notes) == sorted(BASS)
    assert tracks["drums"].drums and len(tracks["drums"].notes) == len(DRUMS)


def test_song_home_takes_it_as_a_tab_and_each_box_finds_its_track(gp5):
    assert sheets.kind_of(gp5, "guitar") == "tab" and sheets.kind_of(gp5, "drums") == "tab"
    picks = {inst: sheets.pick_tracks(gp5, inst) for inst in ("guitar", "bass", "drums")}
    assert [list(p.values()) for p in picks.values()] == [["guitar"], ["bass"], ["drums"]]
    assert len({next(iter(p)) for p in picks.values()}) == 3  # three different tracks


def test_one_file_in_every_box_drives_every_part(tmp_path, gp5):
    for box in ("guitar", "bass", "drums"):
        (tmp_path / "sheets" / box).mkdir(parents=True)
        shutil.copy(gp5, tmp_path / "sheets" / box / gp5.name)
    (tmp_path / "forge").mkdir()
    plan = forge.Plan(tmp_path, {"sheets": {}})
    assert set(plan.refs) == {"guitar", "bass", "drums"} and set(plan.fidelity.values()) == {"seguir"}


def test_copies_of_one_file_are_lined_up_once(monkeypatch, tmp_path, gp5):
    import ai_charter.reference as reference

    calls = []

    def fake_load_guides(path, mapping, y, sr, tempo, rules, log=print, fidelity="seguir", sections=None, anchors=None):
        calls.append(dict(mapping))
        return {inst: f"guide:{inst}" for inst in mapping.values()}

    copies = {}
    for box in ("guitar", "drums"):
        (tmp_path / box).mkdir()
        copies[box] = Path(shutil.copy(gp5, tmp_path / box / gp5.name))
    monkeypatch.setattr(reference, "load_guides", fake_load_guides)
    forge.install_references(copies)
    guides = reference.load_guides(None, None, None, 0, None, None, log=lambda m: None)
    assert len(calls) == 1 and set(calls[0].values()) == {"guitar", "drums"}
    assert set(guides) == {"guitar", "drums"}


def test_one_file_fills_every_box_it_has_an_instrument_for(tmp_path, gp5):
    import inspect_sheet

    (tmp_path / "sheets" / "guitar").mkdir(parents=True)
    shutil.copy(gp5, tmp_path / "sheets" / "guitar" / gp5.name)
    filled = inspect_sheet.inspect(tmp_path, "guitar", gp5.name)
    assert sorted(filled) == ["bass", "drums"]
    assert (tmp_path / "sheets" / "bass" / gp5.name).exists() and (tmp_path / "sheets" / "drums" / gp5.name).exists()
    cache = (tmp_path / "sheets" / ".tracks" / f"{gp5.name}.tsv").read_text().splitlines()
    assert any(l.startswith("#default\tguitar\t") and l.endswith("\t1") for l in cache)


def test_a_guitar_only_file_fills_only_the_guitar(tmp_path, fmt):
    import inspect_sheet

    (tmp_path / "sheets" / "drums").mkdir(parents=True)
    write_gp5(tmp_path / "sheets" / "drums" / f"riff.{fmt}", GUITAR)  # dropped on Drums by mistake
    assert inspect_sheet.inspect(tmp_path, "drums", f"riff.{fmt}") == ["guitar"]


def test_the_solo_guitar_wins_and_unclear_guitars_ask(tmp_path, fmt):
    rhythm = [(b, 40) for b in (0.0, 1.0, 2.0, 3.0)]
    named = write_gp5(tmp_path / f"named.{fmt}", rhythm, guitar_name="Rhythm Guitar", more_guitars=[("Lead Guitar", GUITAR)])
    tracks = sheets.score_tracks(named)
    lead = next(t["index"] for t in tracks if t["name"] == "Lead Guitar")
    assert sheets.default_track(tracks, "guitar") == (lead, True)
    assert sheets.pick_tracks(named, "guitar") == {lead: "guitar"}

    unclear = write_gp5(tmp_path / f"unclear.{fmt}", rhythm, guitar_name="Distortion Guitar", more_guitars=[("Clean Guitar", GUITAR)])
    tracks = sheets.score_tracks(unclear)
    index, sure = sheets.default_track(tracks, "guitar")
    assert not sure
    other = next(t["index"] for t in tracks if t["index"] != index and t["guess"] == "guitar")
    assert sheets.pick_tracks(unclear, "guitar", chosen=other) == {other: "guitar"}  # the user's pick wins
    assert sheets.chosen_track({"sheets": {"guitar_track": f"unclear.{fmt}#{other}"}}, "guitar", f"unclear.{fmt}") == other
