"""Guitar/bass: bars that repeat (a riff, palm-muted chugs) come out the same, like the
drums (charting/drum_repeats.py). Off by default ([detection.<instrument>] repeat_similarity)."""

import numpy as np

from ai_charter.charting.common import Candidate
from ai_charter.charting.drum_repeats import consolidate_pitched

RES = 192
STEP = RES // 4  # a 16th note


def riff_bar(bar: int, skip: set[int] = frozenset(), extra: dict[int, float] | None = None) -> list[Candidate]:
    """16th-note chugs on E2 (40), with an A2 (45) accent on beat 3; sustains of a 16th."""
    out = []
    for q in range(16):
        if q in skip:
            continue
        pitch = 45.0 if q == 8 else 40.0
        tick = bar * 4 * RES + q * STEP
        out.append(Candidate(tick, 0.8, end_tick=tick + STEP, pitch=pitch, voices=2, chord=3))
    for q, pitch in (extra or {}).items():
        tick = bar * 4 * RES + q * STEP
        out.append(Candidate(tick + 0, 0.3, end_tick=tick + STEP, pitch=pitch))
    return out


def pattern(cands: list[Candidate], bar: int) -> list[tuple[int, float]]:
    start = bar * 4 * RES
    return sorted(((c.tick - start) // STEP, c.pitch) for c in cands if start <= c.tick < start + 4 * RES)


def test_a_missed_chug_and_a_stray_note_are_fixed_by_the_other_repeats():
    cands = riff_bar(0) + riff_bar(1, skip={5, 6}) + riff_bar(2) + riff_bar(3, extra={13: 52.0})
    cands.sort(key=lambda c: c.tick)
    out = consolidate_pitched(cands, np.arange(5) * 4 * RES, RES, similarity=0.8)
    want = pattern(riff_bar(0), 0)
    for bar in range(4):
        assert pattern(out, bar) == want


def test_notes_keep_their_chord_and_length_when_copied():
    cands = riff_bar(0) + riff_bar(1, skip={5}) + riff_bar(2)
    out = consolidate_pitched(sorted(cands, key=lambda c: c.tick), np.arange(4) * 4 * RES, RES, similarity=0.8)
    filled = [c for c in out if c.tick == 1 * 4 * RES + 5 * STEP]
    assert len(filled) == 1
    assert filled[0].chord == 3 and filled[0].voices == 2 and filled[0].end_tick - filled[0].tick == STEP


def test_different_riffs_are_left_alone():
    a = riff_bar(0)
    b = [Candidate(4 * RES + q * STEP, 0.8, end_tick=4 * RES + q * STEP + STEP, pitch=50.0 + q % 3)
         for q in range(0, 16, 2)]
    out = consolidate_pitched(sorted(a + b, key=lambda c: c.tick), np.arange(3) * 4 * RES, RES, similarity=0.8)
    assert pattern(out, 0) == pattern(a, 0) and pattern(out, 1) == pattern(b, 1)


def test_a_misread_pitch_takes_the_pitch_the_other_repeats_agree_on():
    bars = [riff_bar(b) for b in range(4)]
    bars[2] = [replace_pitch(c, 47.0) if (c.tick - 2 * 4 * RES) // STEP == 3 else c for c in bars[2]]
    cands = sorted(sum(bars, []), key=lambda c: c.tick)
    out = consolidate_pitched(cands, np.arange(5) * 4 * RES, RES, similarity=0.8)
    assert pattern(out, 2) == pattern(riff_bar(0), 0)


def test_triplet_bars_repeat_too():
    def triplet_bar(bar, skip=()):
        step = RES // 3  # 8th-note triplets
        return [Candidate(bar * 4 * RES + q * step, 0.8, end_tick=bar * 4 * RES + q * step + step, pitch=40.0)
                for q in range(12) if q not in skip]
    cands = sorted(triplet_bar(0) + triplet_bar(1, skip={4}) + triplet_bar(2), key=lambda c: c.tick)
    out = consolidate_pitched(cands, np.arange(4) * 4 * RES, RES, similarity=0.8)
    assert len([c for c in out if 4 * RES <= c.tick < 8 * RES]) == 12


def test_same_rhythm_on_other_notes_is_another_riff():
    a = [riff_bar(b) for b in range(3)]
    shifted = [Candidate(c.tick + 3 * 4 * RES, c.strength, end_tick=c.end_tick + 3 * 4 * RES, pitch=c.pitch + 5)
               for c in riff_bar(0)]  # the same chugs a 4th up (the chorus on A)
    cands = sorted(sum(a, []) + shifted, key=lambda c: c.tick)
    out = consolidate_pitched(cands, np.arange(5) * 4 * RES, RES, similarity=0.8)
    assert pattern(out, 3) == pattern(shifted, 3)


def replace_pitch(c: Candidate, pitch: float) -> Candidate:
    from dataclasses import replace
    return replace(c, pitch=pitch)


def test_a_riff_with_noisy_pitches_still_repeats():
    """Distorted power chords: the detected pitch of each chug wanders between the notes
    of the chord (E, B, the octave E); the rhythm and the notes in play are the riff."""
    rng = np.random.default_rng(0)
    cands = []
    for b in range(6):
        for q in range(16):
            if b == 4 and q in (6, 7):
                continue  # two chugs missed in one repeat
            tick = b * 4 * RES + q * STEP
            cands.append(Candidate(tick, 0.8, end_tick=tick + STEP, pitch=float(rng.choice([40, 47, 52]))))
    out = consolidate_pitched(sorted(cands, key=lambda c: c.tick), np.arange(7) * 4 * RES, RES, similarity=0.8)
    pats = [pattern(out, b) for b in range(6)]
    assert all(p == pats[0] for p in pats) and len(pats[0]) == 16
