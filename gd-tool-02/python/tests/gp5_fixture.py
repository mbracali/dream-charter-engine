"""Writes real Guitar Pro 5 files for the tests (pyguitarpro's writer): a guitar track (and
optionally a bass and a drum track) from (beat, MIDI pitch) notes on a 16th-note grid."""

from __future__ import annotations

from pathlib import Path

import guitarpro as gp

GUITAR = [64, 59, 55, 50, 45, 40]  # E standard, string 1 (high e) first
BASS = [43, 38, 33, 28]


def _place(pitch: int, tuning: list[int], used: set[int]) -> tuple[int, int] | None:
    """(string number 1.., fret) for a pitch: the lowest fret on a free string."""
    best = None
    for i, open_pitch in enumerate(tuning):
        fret = pitch - open_pitch
        if 0 <= fret <= 24 and (i + 1) not in used and (best is None or fret < best[1]):
            best = (i + 1, fret)
    return best


def _fill(track, notes: list[tuple[float, int]], bars: int, tuning: list[int] | None, drums: bool) -> None:
    by_slot: dict[int, list[int]] = {}
    for beat, pitch in notes:
        by_slot.setdefault(int(round(beat * 4)), []).append(pitch)
    for bar in range(bars):
        voice = track.measures[bar].voices[0]
        voice.beats.clear()
        for slot in range(bar * 16, bar * 16 + 16):
            beat = gp.Beat(voice, duration=gp.Duration(value=16))
            pitches = by_slot.get(slot, [])
            if not pitches:
                beat.status = gp.BeatStatus.rest
            else:
                beat.status = gp.BeatStatus.normal
                used: set[int] = set()
                for p in sorted(pitches):
                    if drums:
                        string, fret = 1 + len(used), p  # drums: the fret is the GM drum note
                    else:
                        placed = _place(p, tuning, used)
                        if placed is None:
                            continue
                        string, fret = placed
                    used.add(string)
                    beat.notes.append(gp.Note(beat, value=fret, string=string, type=gp.NoteType.normal))
            voice.beats.append(beat)


def write_gp5(path: Path, guitar: list[tuple[float, int]], bpm: int = 140,
              bass: list[tuple[float, int]] | None = None, drums: list[tuple[float, int]] | None = None) -> Path:
    """`guitar` / `bass` / `drums`: (beat from the start, MIDI pitch or GM drum note)."""
    beats = max([b for b, _ in guitar + (bass or []) + (drums or [])] + [0.0])
    bars = int(beats // 4) + 1
    song = gp.Song()
    song.tempo = bpm
    for _ in range(bars - 1):
        song.addMeasureHeader(gp.MeasureHeader())
    tracks = [("Distortion Guitar", GUITAR, guitar, False, 30)]
    if bass:
        tracks.append(("Bass", BASS, bass, False, 33))
    if drums:
        tracks.append(("Drums", None, drums, True, 0))
    while len(song.tracks) < len(tracks):
        song.tracks.append(gp.Track(song, number=len(song.tracks) + 1))
    for track in song.tracks:  # one measure per bar in every track
        for header in song.measureHeaders[len(track.measures):]:
            track.measures.append(gp.Measure(track, header))
    for track, (name, tuning, notes, is_drums, program) in zip(song.tracks, tracks):
        track.name = name
        track.isPercussionTrack = is_drums
        if tuning:
            track.strings = [gp.GuitarString(i + 1, p) for i, p in enumerate(tuning)]
        else:
            track.strings = [gp.GuitarString(i + 1, 0) for i in range(6)]
        track.channel.instrument = program
        if is_drums:
            track.channel.channel = 9
            track.channel.effectChannel = 9
        _fill(track, notes, bars, tuning, is_drums)
    gp.write(song, str(path), version=(5, 1, 0))
    return path
