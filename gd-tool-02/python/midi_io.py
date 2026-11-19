"""notes.mid (Rock Band layout, read by Clone Hero and YARG) <-> chart_text.Chart.

Rock Band layout: Easy 60-64, Medium 72-76, Hard 84-88, Expert 96-100 (green..orange
or kick, red, yellow, blue, green); +5 forces HOPO, +6 forces strum; 116 Star Power;
drums 110-112 make yellow/blue/green toms (cymbals otherwise); PART VOCALS 36-84 with
a lyric at each note, 105/106 phrases.
"""

from __future__ import annotations

import bisect
from pathlib import Path

import mido

from chart_text import Chart, Note, Vocal

PART_TRACKS = {"PART GUITAR": "guitar", "PART BASS": "bass", "PART KEYS": "keys", "PART DRUMS": "drums"}
TRACK_OF = {v: k for k, v in PART_TRACKS.items()}
DIFF_BASE = {0: 60, 1: 72, 2: 84, 3: 96}  # Easy, Medium, Hard, Expert
STAR_POWER, PHRASE, PHRASE_2 = 116, 105, 106
TOM_MARKER = {2: 110, 3: 111, 4: 112}
VOCAL_RANGE = range(36, 85)


def absolute(track):
    tick = 0
    for msg in track:
        tick += msg.time
        yield tick, msg


class TempoMap:
    """Ticks <-> seconds from the tempo and time-signature events of track 0."""

    def __init__(self, midi: mido.MidiFile) -> None:
        self.res = midi.ticks_per_beat
        changes: list[tuple[int, int]] = []  # (tick, microseconds per beat)
        meters: list[tuple[int, int]] = []   # (tick, beats per bar)
        for tick, msg in absolute(midi.tracks[0]):
            if msg.type == "set_tempo":
                changes.append((tick, msg.tempo))
            elif msg.type == "time_signature":
                meters.append((tick, msg.numerator))
        # By tick only (stable): two events at one tick keep their file order. The MIDI
        # defaults (120 BPM, 4/4) only count when the file sets nothing at tick 0.
        changes.sort(key=lambda c: c[0])
        meters.sort(key=lambda m: m[0])
        if not changes or changes[0][0] != 0:
            changes.insert(0, (0, 500000))
        if not meters or meters[0][0] != 0:
            meters.insert(0, (0, 4))
        self.changes, self.meters = changes, meters
        self._ticks = [c[0] for c in changes]
        self._secs = [0.0]
        for (t0, tempo), (t1, _) in zip(changes, changes[1:]):
            self._secs.append(self._secs[-1] + (t1 - t0) * tempo / 1e6 / self.res)

    def seconds(self, tick: float) -> float:
        i = max(0, bisect.bisect_right(self._ticks, tick) - 1)
        return self._secs[i] + (tick - self._ticks[i]) * self.changes[i][1] / 1e6 / self.res

    def tick(self, seconds: float) -> int:
        i = max(0, bisect.bisect_right(self._secs, seconds) - 1)
        return int(round(self._ticks[i] + (seconds - self._secs[i]) * 1e6 * self.res / self.changes[i][1]))


def spans(track, pitches: set[int] | range) -> list[tuple[int, int, int]]:
    """(start, end, pitch) for the note-on/off pairs of the given pitches."""
    open_notes: dict[int, int] = {}
    out = []
    for tick, msg in absolute(track):
        if msg.type not in ("note_on", "note_off") or msg.note not in pitches:
            continue
        if msg.type == "note_on" and msg.velocity > 0:
            open_notes[msg.note] = tick
        elif msg.note in open_notes:
            out.append((open_notes.pop(msg.note), tick, msg.note))
    return out


def inside(tick: int, ranges: list[tuple[int, int, int]]) -> bool:
    return any(a <= tick < b for a, b, _ in ranges)


def natural_hopo(tick: int, lanes: list[int], prev_tick: int | None, prev_lanes: list[int] | None, res: int) -> bool:
    """Clone Hero's rule: a single note close after a different note is a HOPO."""
    return prev_tick is not None and tick - prev_tick <= res // 3 and len(lanes) == 1 and lanes != prev_lanes


def track_name(track) -> str:
    return next((m.name for m in track if m.type == "track_name"), track.name or "").strip()


def fret_notes(track, part: str, tempo: TempoMap) -> list[Note]:
    res, drums, out = tempo.res, part == "drums", []
    star = spans(track, {STAR_POWER})
    toms = {lane: spans(track, {pitch}) for lane, pitch in TOM_MARKER.items()}
    for diff, base in DIFF_BASE.items():
        forced_hopo, forced_strum = spans(track, {base + 5}), spans(track, {base + 6})
        by_tick: dict[int, list[tuple[int, int]]] = {}
        for a, b, p in spans(track, range(base, base + 5)):
            by_tick.setdefault(a, []).append((b, p - base))
        prev_tick, prev_lanes = None, None
        for a in sorted(by_tick):
            group = by_tick[a]
            lanes = sorted(lane for _, lane in group)
            hopo = (not drums and (natural_hopo(a, lanes, prev_tick, prev_lanes, res) or inside(a, forced_hopo))
                    and not inside(a, forced_strum))
            for b, lane in group:
                flags = ("h" if hopo else "") + ("s" if inside(a, star) else "")
                if drums and lane >= 2 and not inside(a, toms[lane]):
                    flags += "c"
                start = tempo.seconds(a)
                # Short notes are taps; only notes longer than ~1/3 beat keep a sustain.
                sustain = tempo.seconds(b) - start if (b - a) > res // 3 and not drums else 0.0
                out.append(Note(part, diff, start, sustain, lane, flags))
            prev_tick, prev_lanes = a, lanes
    return out


def midi_to_chart(path: Path, duration: float = 0.0) -> Chart:
    midi = mido.MidiFile(str(path))
    tempo = TempoMap(midi)
    chart = Chart(duration=duration)
    end_tick = max(sum(m.time for m in tr) for tr in midi.tracks)
    # Beats and bars from the tempo map and the time signatures.
    tick, beat_in_bar, meter_i = 0, 0, 0
    while tick <= end_tick:
        while meter_i + 1 < len(tempo.meters) and tempo.meters[meter_i + 1][0] <= tick:
            meter_i += 1
            beat_in_bar = 0
        chart.beats.append((tempo.seconds(tick), beat_in_bar == 0))
        beat_in_bar = (beat_in_bar + 1) % max(1, tempo.meters[meter_i][1])
        tick += tempo.res
    for track in midi.tracks:
        name = track_name(track)
        if name == "EVENTS":
            for t, msg in absolute(track):
                if msg.type == "text" and msg.text.startswith("[section"):
                    chart.sections.append((tempo.seconds(t), msg.text[9:].rstrip("]").strip()))
                elif msg.type == "text" and msg.text.startswith("[prc_"):
                    chart.sections.append((tempo.seconds(t), msg.text[5:].rstrip("]").replace("_", " ").strip()))
        elif name in PART_TRACKS:
            chart.notes += fret_notes(track, PART_TRACKS[name], tempo)
        elif name == "PART VOCALS":
            lyric_at: dict[int, str] = {}
            for t, msg in absolute(track):
                if msg.type in ("lyrics", "text") and not msg.text.startswith("["):
                    lyric_at[t] = msg.text
            for a, b, p in spans(track, VOCAL_RANGE):
                ta = tempo.seconds(a)
                chart.vocals.append(Vocal(ta, tempo.seconds(b) - ta, p, lyric_at.get(a, "")))
            for a, b, _ in spans(track, {PHRASE, PHRASE_2}):
                chart.phrases.append((tempo.seconds(a), tempo.seconds(b)))
    if not chart.duration:
        chart.duration = tempo.seconds(end_tick)
    return chart


def midi_parts(path: Path) -> set[str]:
    """The playable parts a MIDI file has (PART GUITAR -> guitar, ...); empty for a score."""
    try:
        midi = mido.MidiFile(str(path))
    except Exception:  # noqa: BLE001 - not a readable MIDI: no parts
        return set()
    found = {PART_TRACKS[track_name(t)] for t in midi.tracks if track_name(t) in PART_TRACKS}
    if any(track_name(t) == "PART VOCALS" for t in midi.tracks):
        found.add("vocals")
    return found


# ---- Chart -> notes.mid (Export) -------------------------------------------------------

class _Track:
    def __init__(self, name: str) -> None:
        self.name = name
        self.events: list[tuple[int, int, mido.Message | mido.MetaMessage]] = []

    def note(self, tick: int, length: int, pitch: int) -> None:
        # At one tick, note_off (order 0) comes before note_on (order 2).
        self.events.append((tick, 2, mido.Message("note_on", note=pitch, velocity=100, time=0)))
        self.events.append((tick + max(1, length), 0, mido.Message("note_off", note=pitch, velocity=0, time=0)))

    def meta(self, tick: int, kind: str, **kwargs) -> None:
        self.events.append((tick, 1, mido.MetaMessage(kind, time=0, **kwargs)))

    def build(self) -> mido.MidiTrack:
        track = mido.MidiTrack()
        track.append(mido.MetaMessage("track_name", name=safe_text(self.name), time=0))
        now = 0
        for tick, _, msg in sorted(self.events, key=lambda e: (e[0], e[1])):
            track.append(msg.copy(time=tick - now))
            now = tick
        track.append(mido.MetaMessage("end_of_track", time=0))
        return track


def safe_text(text: str) -> str:
    """MIDI text is Latin-1 in the Rock Band format (covers Portuguese accents)."""
    return text.encode("latin-1", errors="replace").decode("latin-1")


def _star_spans(notes: list[Note], tempo: TempoMap, short: int) -> list[tuple[int, int]]:
    """Star Power phrases from the starred Expert chords: runs of starred chords, split by
    any chord without the star."""
    by_tick: dict[int, list[Note]] = {}
    for n in notes:
        by_tick.setdefault(tempo.tick(n.t), []).append(n)
    out, start, end = [], None, None
    for tick in sorted(by_tick):
        group = by_tick[tick]
        if all(n.star for n in group):
            length = max(tempo.tick(n.t + n.sustain) - tick if n.sustain > 0 else short for n in group)
            start = tick if start is None else start
            end = max(end or 0, tick + length)
        elif start is not None:
            out.append((start, end - start))
            start, end = None, None
    if start is not None:
        out.append((start, end - start))
    return out


def fret_track(part: str, notes: list[Note], tempo: TempoMap) -> mido.MidiTrack:
    res, drums = tempo.res, part == "drums"
    short = max(1, res // 16)
    out = _Track(TRACK_OF[part])
    toms: set[tuple[int, int]] = set()
    for diff, base in DIFF_BASE.items():
        by_tick: dict[int, list[Note]] = {}
        for n in notes:
            if n.diff == diff:
                by_tick.setdefault(tempo.tick(n.t), []).append(n)
        prev_tick, prev_lanes = None, None
        for tick in sorted(by_tick):
            group = by_tick[tick]
            lanes = sorted({n.lane for n in group})
            for n in group:
                length = tempo.tick(n.t + n.sustain) - tick if n.sustain > 0 and not drums else short
                out.note(tick, max(short, length), base + max(0, min(4, n.lane)))
                if drums and n.lane in TOM_MARKER and not n.cymbal:
                    toms.add((tick, TOM_MARKER[n.lane]))
            if not drums:
                want = any(n.hopo for n in group)
                if want != natural_hopo(tick, lanes, prev_tick, prev_lanes, res):
                    out.note(tick, short, base + (5 if want else 6))
            prev_tick, prev_lanes = tick, lanes
    for tick, pitch in sorted(toms):
        out.note(tick, short, pitch)
    expert = [n for n in notes if n.diff == 3] or notes
    for tick, length in _star_spans(expert, tempo, short):
        out.note(tick, length, STAR_POWER)
    return out.build()


def vocal_track(chart: Chart, tempo: TempoMap) -> mido.MidiTrack:
    out = _Track("PART VOCALS")
    for v in chart.vocals:
        tick = tempo.tick(v.t)
        if v.lyric:
            out.meta(tick, "lyrics", text=safe_text(v.lyric))
        out.note(tick, max(1, tempo.tick(v.t + v.dur) - tick), max(36, min(84, v.pitch)))
    for a, b in vocal_phrases(chart):
        ta = tempo.tick(a)
        out.note(ta, max(1, tempo.tick(b) - ta), PHRASE)
    return out.build()


def vocal_phrases(chart: Chart) -> list[tuple[float, float]]:
    """The chart's phrases, plus new ones around syllables no phrase covers (added or
    moved in Fine tuning): the games only show the notes inside a phrase."""
    phrases = sorted(chart.phrases)
    loose = []
    for v in sorted(chart.vocals, key=lambda v: v.t):
        if any(a - 1e-3 <= v.t < b for a, b in phrases):
            continue
        # Right next to a phrase (under 0.6 s): stretch it; else it needs a new one.
        near = [k for k, (a, b) in enumerate(phrases) if b - 1e-3 <= v.t < b + 0.6 or a - 0.6 < v.t + v.dur <= a + 1e-3]
        if near:
            a, b = phrases[near[0]]
            phrases[near[0]] = (min(a, v.t), max(b, v.t + v.dur))
        else:
            loose.append(v)
    extra = [(a, b) for a, b in _phrases_from_gaps(loose) if not any(pa < b and a < pb for pa, pb in phrases)]
    return sorted(phrases + extra)


def _phrases_from_gaps(vocals: list[Vocal], gap: float = 1.2) -> list[tuple[float, float]]:
    out: list[tuple[float, float]] = []
    for v in sorted(vocals, key=lambda v: v.t):
        if out and v.t - out[-1][1] < gap:
            out[-1] = (out[-1][0], v.t + v.dur)
        else:
            out.append((v.t, v.t + v.dur))
    return out


def chart_to_midi(chart: Chart, base_midi: Path, out: Path, parts: list[str], vocals: bool) -> None:
    """Writes `out` with base_midi's tempo map (track 0) and the chart's parts."""
    base = mido.MidiFile(str(base_midi))
    tempo = TempoMap(base)
    midi = mido.MidiFile(type=1, ticks_per_beat=base.ticks_per_beat)
    midi.tracks.append(base.tracks[0].copy())
    events = _Track("EVENTS")
    for i, (t, name) in enumerate(sorted(chart.sections)):
        events.meta(tempo.tick(t), "text", text=f"[section {safe_text(name) or f'Section {i + 1}'}]")
    last = max([n.t + n.sustain for n in chart.notes] + [v.t + v.dur for v in chart.vocals] + [0.0])
    end_tick = max(tempo.tick(chart.duration), tempo.tick(last) + tempo.res)
    events.meta(end_tick, "text", text="[end]")
    midi.tracks.append(events.build())
    for part in parts:
        notes = chart.part(part)
        if notes:
            midi.tracks.append(fret_track(part, notes, tempo))
    if vocals and chart.vocals:
        midi.tracks.append(vocal_track(chart, tempo))
    midi.save(str(out))
