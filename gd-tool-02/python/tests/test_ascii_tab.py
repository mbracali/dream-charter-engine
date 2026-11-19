"""Text (ASCII) tabs -> MIDI scores, and how the sheets tell them from chord sheets."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ascii_tab  # noqa: E402
import sheets  # noqa: E402

GUITAR = """Intro riff
e|-----------------|-----------------|
B|-----------------|-----------------|
G|-----------------|-----------------|
D|-----------------|-----------------|
A|-------2---------|-----------------|
E|-0---0---0-3-0---|-0-0-0-0---------|   x2
"""

DROP_D = """e|------|
B|------|
G|------|
D|------|
A|------|
D|-0----|
"""

BASS = """G|----------------|
D|----------------|
A|----------------|
E|-0-0-0-0-3-3-5-5|
"""

DRUMS = """HH|x-x-x-x-x-x-x-x-|
SD|----o-------o---|
BD|o-------o-o-----|
"""

CHORDS = """[Intro] Em  C  D  Em

Em            C
Some words of a verse here
D               Em
More words to sing
"""


def test_tabs_and_chord_sheets_are_told_apart():
    assert ascii_tab.is_ascii_tab(GUITAR) and ascii_tab.is_ascii_tab(DRUMS) and ascii_tab.is_ascii_tab(BASS)
    assert not ascii_tab.is_ascii_tab(CHORDS)


def test_guitar_tab_pitches_rhythm_and_repeat():
    tab = ascii_tab.parse(GUITAR)
    assert not tab.drums
    first_bar = [(n.beat, n.pitch) for n in tab.notes if n.beat < 4]
    # Low E string open = E2 (40), 3rd fret = G2 (43); A string 2nd fret = B2 (47). The bar's
    # leading dash is padding; notes land on 16ths.
    assert first_bar == [(0.0, 40), (1.0, 40), (1.5, 47), (2.0, 40), (2.5, 43), (3.0, 40)]
    assert tab.beats == 16.0  # two bars, played twice
    assert len(tab.notes) == 2 * 10


def test_tuning_comes_from_the_labels():
    tab = ascii_tab.parse(DROP_D)
    assert [n.pitch for n in tab.notes] == [38]  # drop D low string: D2


def test_bass_has_four_strings():
    tab = ascii_tab.parse(BASS)
    assert [n.pitch for n in tab.notes][:5] == [28, 28, 28, 28, 31]


def test_drum_tab_pieces():
    tab = ascii_tab.parse(DRUMS)
    assert tab.drums
    by_piece = {}
    for n in tab.notes:
        by_piece.setdefault(n.pitch, []).append(n.beat)
    assert len(by_piece[42]) == 8 and by_piece[38] == [1.0, 3.0] and by_piece[36] == [0.0, 2.0, 2.5]


def test_the_score_is_read_by_the_engine(tmp_path):
    from ai_charter.reference import read_score

    for name, text, instrument in (("g", GUITAR, "guitar"), ("b", BASS, "bass"), ("d", DRUMS, "drums")):
        out = tmp_path / f"{name}.mid"
        assert ascii_tab.to_midi(text, out, instrument) > 0
        score = read_score(out)
        track = next(t for t in score.tracks if t.notes)
        assert track.guess == instrument, (instrument, track.guess)
        assert sheets.pick_tracks(out, instrument) == {track.index: instrument}


def test_sheet_kinds_read_the_text(tmp_path):
    (tmp_path / "riff.txt").write_text(GUITAR)
    (tmp_path / "chords.txt").write_text(CHORDS)
    (tmp_path / "groove.tab").write_text(DRUMS)
    assert sheets.kind_of(tmp_path / "riff.txt", "guitar") == "texttab"
    assert sheets.kind_of(tmp_path / "chords.txt", "guitar") == "chords"
    assert sheets.kind_of(tmp_path / "groove.tab", "drums") == "texttab"
