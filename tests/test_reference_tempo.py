"""A score's written tempo helps line it up: when it agrees with the recording, only that
speed is tried (a partial tab is not stretched at half speed over the whole song)."""

import sys
from pathlib import Path

import mido
import numpy as np
import pytest

from ai_charter.reference import alignment_factors, read_score


def beats(bpm, n=200):
    return np.arange(n) * 60.0 / bpm


def test_the_written_tempo_picks_the_speed():
    assert alignment_factors(127, beats(126.2)) == (1.0,)      # same tempo
    assert alignment_factors(254, beats(126.2)) == (0.5,)      # score written at double speed
    assert alignment_factors(63, beats(126.2)) == (2.0,)       # at half speed
    assert alignment_factors(None, beats(126.2)) == (1.0, 2.0, 0.5)   # no tempo: all three
    assert alignment_factors(100, beats(140)) == (1.0, 2.0, 0.5)      # no agreement: all three


def test_midi_scores_carry_their_tempo(tmp_path):
    mid = mido.MidiFile(type=1, ticks_per_beat=480)
    track = mido.MidiTrack([mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(127), time=0),
                            mido.Message("program_change", program=30, time=0),
                            mido.Message("note_on", note=52, velocity=90, time=0),
                            mido.Message("note_off", note=52, velocity=0, time=480)])
    mid.tracks.append(track)
    mid.save(tmp_path / "riff.mid")
    assert read_score(tmp_path / "riff.mid").bpm == pytest.approx(127, abs=0.01)


def test_guitar_pro_scores_carry_their_tempo(tmp_path):
    pytest.importorskip("guitarpro")
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gd-tool-02" / "python" / "tests"))
    from gp5_fixture import write_gp5

    for ext in ("gp4", "gp5"):
        path = write_gp5(tmp_path / f"song.{ext}", [(0.0, 52), (1.0, 55)], bpm=127)
        assert read_score(path).bpm == 127


def test_a_partial_score_is_judged_on_the_part_it_covers():
    from ai_charter.reference import _in_span

    assert _in_span((100, 200), 150) and not _in_span((100, 200), 250) and _in_span(None, 10_000)


def test_the_song_starts_where_the_sound_starts():
    from ai_charter.reference import _first_sound_beat

    sr = 22050
    y = np.concatenate([np.zeros(sr * 2), 0.5 * np.sin(np.arange(sr * 4) * 0.05)])  # 2 s of silence
    grid = np.arange(0, 6.01, 0.5)   # a beat every 0.5 s
    assert _first_sound_beat(y, sr, grid) == pytest.approx(4.0, abs=0.1)   # beat 4 = 2.0 s
