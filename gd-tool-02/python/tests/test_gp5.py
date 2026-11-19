"""Guitar Pro 5 sheets (R31): a real .gp5 written with pyguitarpro is read by the engine,
each instrument finds its own track, and the same file in several boxes is lined up once."""

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


@pytest.fixture
def gp5(tmp_path):
    return write_gp5(tmp_path / "song.gp5", GUITAR, 140, bass=BASS, drums=DRUMS)


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
        shutil.copy(gp5, tmp_path / "sheets" / box / "song.gp5")
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
        copies[box] = Path(shutil.copy(gp5, tmp_path / box / "song.gp5"))
    monkeypatch.setattr(reference, "load_guides", fake_load_guides)
    forge.install_references(copies)
    guides = reference.load_guides(None, None, None, 0, None, None, log=lambda m: None)
    assert len(calls) == 1 and set(calls[0].values()) == {"guitar", "drums"}
    assert set(guides) == {"guitar", "drums"}
