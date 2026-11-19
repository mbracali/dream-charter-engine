"""Drums from a sheet (gd-tool-02 R41-R43): the hits the recording adds to the sheet's, the
hi-hat pedal left out, and a sheet longer than the recording kept on its grid."""

import numpy as np

from ai_charter.charting.common import ChartNote, TrackChart
from ai_charter.config import load_rules
from ai_charter.reference import GM_DRUMS, Alignment, DrumEvent, DrumGuide, straighten_ends

RES, BAR = 192, 4 * 192


def _guide_and_track():
    # The sheet: kick on 1 and 3, snare on 2 and 4, two bars. The recording also has a snare on the
    # "and" of 4 (a fill the tab left out), and a kick 1/32 off the sheet's kick (the same hit).
    events = [DrumEvent(t, frozenset({"kick"} if (t // RES) % 2 == 0 else {"snare"})) for t in range(0, 2 * BAR, RES)]
    detected = [ChartNote(t, (0,) if (t // RES) % 2 == 0 else (1,)) for t in range(0, 2 * BAR, RES)]
    detected += [ChartNote(3 * RES + RES // 2, (1,)), ChartNote(BAR + RES // 8, (0,))]
    detected += [ChartNote(BAR + RES // 2, (2,), flags=(66,))]   # a hi-hat only the recording has: the pedal
    return events, TrackChart(notes={"expert": sorted(detected, key=lambda n: n.tick)})


def test_hits_heard_in_the_recording_are_added_where_the_sheet_has_none():
    events, track = _guide_and_track()
    lines = []
    out = DrumGuide(events, RES, "seguir", load_rules(), add_heard=True)(track, log=lines.append, sp_windows=[])
    expert = {n.tick: n.lanes for n in out.notes["expert"]}
    assert expert[3 * RES + RES // 2] == (1,)          # the fill is in
    assert BAR + RES // 8 not in expert                # the near-duplicate kick is not
    assert BAR + RES // 2 not in expert                # nor a cymbal (a hi-hat pedal chick)
    assert len(expert) == 8 + 1
    assert "1 golpe(s) ouvidos na gravação somados" in lines[0]


def test_without_it_the_sheet_alone_drives_the_drums():
    events, track = _guide_and_track()
    out = DrumGuide(events, RES, "seguir", load_rules())(track, log=lambda _l: None, sp_windows=[])
    assert sorted(n.tick for n in out.notes["expert"]) == list(range(0, 2 * BAR, RES))


def test_the_hihat_pedal_is_the_foot_not_a_pad():
    assert 44 not in GM_DRUMS and GM_DRUMS[42] == GM_DRUMS[46] == "hihat"


def test_a_sheet_longer_than_the_recording_stays_on_its_grid():
    # The alignment path stalls: score beats 0-3 (a count-in) all on recording beat 0, and score
    # beats 40-48 (an ending the recording does not have) all on recording beat 36.
    ref = np.arange(0, 49, dtype=float)
    audio = np.clip(ref - 4, 0, 36)
    fixed = straighten_ends(ref, audio, 1.0)
    assert np.allclose(fixed, ref - 4)
    a = Alignment(ref, fixed, 0, 1.0, 0.1, audio_last=37.0)
    assert a.to_audio_beat(1.0) is None                # the count-in is before the recording
    assert a.to_audio_beat(20.0) == 16.0
    assert a.to_audio_beat(41.0) == 37.0               # still in the recording
    assert a.to_audio_beat(45.0) is None               # past its end: dropped, not piled up


def test_a_well_aligned_path_is_left_alone():
    ref = np.arange(0, 100, dtype=float)
    audio = ref * 1.0 + np.sin(ref) * 0.2              # small warps, no stall
    assert np.allclose(straighten_ends(ref, np.maximum.accumulate(audio), 1.0), np.maximum.accumulate(audio))
