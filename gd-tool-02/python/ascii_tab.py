"""Text (ASCII) tabs, as pasted from tab sites, -> a MIDI score the engine can follow.

    e|-------0---3-|        guitar / bass: one line per string, high string on top;
    B|-----1-------|        frets are numbers; "|" are bar lines
    G|---0---------|
    D|-2-----------|
    A|-------------|
    E|-------------|   x4

    HH|x-x-x-x-x-x-x-x-|    drums: one line per piece (HH, SD, BD, CC, Rd, T1, T2, FT...),
    SD|----o-------o---|    any letter but "-" is a hit
    BD|o-------o-o-----|

Text tabs have no real rhythm: each bar is spread evenly over 4 beats by the position of
its notes in the columns. That is why forge.py uses them only to correct the pitches and
chords of what the engine hears (fidelity "corrigir"), never its rhythm.

A system (the lines of one staff) followed by "x4" / "4x" / "(x4)" is repeated.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

import mido

NOTE_PCS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
# Standard tunings, high string first (the order tabs print them).
STANDARD = {4: [43, 38, 33, 28], 5: [43, 38, 33, 28, 23], 6: [64, 59, 55, 50, 45, 40], 7: [64, 59, 55, 50, 45, 40, 35]}
DRUMS = [  # label pattern -> General MIDI drum note
    (r"^(hh|h|hc|ch|hi|ho|oh)$", 42), (r"^(cc|c|cr|crash|c1|c2|ch2)$", 49), (r"^(rd|r|ri|ride|rc)$", 51),
    (r"^(sd|s|sn|snare)$", 38), (r"^(bd|b|k|kd|kick|bass)$", 36), (r"^(t1|ht|t|hi-t|tom1)$", 48),
    (r"^(t2|mt|tom2)$", 45), (r"^(ft|t3|lt|f|fl|tom3)$", 41), (r"^(sp|spl|china|cn)$", 52),
]
TAB_LINE = re.compile(r"^\s*([A-Za-z][#b]?\d?|[A-Za-z]{1,5}\d?)\s*([|:])(.*)$")
REPEAT = re.compile(r"(?:\bx\s*(\d{1,2})\b|\b(\d{1,2})\s*x\b|\((?:x\s*)?(\d{1,2})x?\))", re.I)


def _is_tab_body(body: str) -> bool:
    body = body.strip()
    return len(body) >= 4 and body.count("-") >= len(body) * 0.4


def is_ascii_tab(text: str) -> bool:
    """At least 3 tab-looking lines (a label, a bar line, then mostly dashes)."""
    count = 0
    for line in text.splitlines():
        m = TAB_LINE.match(line)
        if m and _is_tab_body(m.group(3)):
            count += 1
    return count >= 3


@dataclass
class System:
    labels: list[str]
    bodies: list[str]
    repeat: int = 1


@dataclass
class TabNote:
    beat: float
    length: float
    pitch: int


@dataclass
class ParsedTab:
    drums: bool
    notes: list[TabNote] = field(default_factory=list)
    beats: float = 0.0


def systems(text: str) -> list[System]:
    out: list[System] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = TAB_LINE.match(lines[i])
        if not (m and _is_tab_body(m.group(3))):
            i += 1
            continue
        labels, bodies, tail = [], [], ""
        while i < len(lines):
            m = TAB_LINE.match(lines[i])
            if not (m and _is_tab_body(m.group(3))):
                break
            labels.append(m.group(1))
            body = m.group(3)
            last_bar = body.rfind("|")
            tail += " " + (body[last_bar + 1:] if last_bar >= 0 else "")
            bodies.append(body[:last_bar + 1] if last_bar >= 0 else body)
            i += 1
        # "x4" after the bar lines, or on the next line.
        if i < len(lines) and not TAB_LINE.match(lines[i]):
            tail += " " + lines[i]
        rep = REPEAT.search(tail)
        repeat = int(next(g for g in rep.groups() if g)) if rep else 1
        out.append(System(labels, bodies, max(1, min(repeat, 16))))
    return out


def _string_pitches(labels: list[str]) -> list[int]:
    base = STANDARD.get(len(labels)) or STANDARD[6][:len(labels)]
    out = []
    for label, std in zip(labels, base):
        name = label[:2] if len(label) > 1 and label[1] in "#b" else label[:1]
        pc = NOTE_PCS.get(name[0].upper())
        if pc is None:
            out.append(std)
            continue
        pc = (pc + (1 if name.endswith("#") else -1 if name.endswith("b") and len(name) > 1 else 0)) % 12
        # The pitch with that name nearest to the standard string (drop D, down-tuning...).
        cands = [p for p in range(std - 6, std + 7) if p % 12 == pc]
        out.append(min(cands, key=lambda p: abs(p - std)) if cands else std)
    return out


def _drum_note(label: str) -> int | None:
    for pattern, note in DRUMS:
        if re.match(pattern, label.lower()):
            return note
    return None


def _looks_like_drums(labels: list[str]) -> bool:
    known = sum(_drum_note(l) is not None for l in labels)
    named = sum(1 for l in labels if re.fullmatch(r"[A-Ga-g][#b]?", l))
    return known > named


def _events(body: str, drums: bool) -> tuple[list[tuple[int, int]], list[int]]:
    """(column, fret or 1 for a drum hit) and the bar-line columns of one line."""
    events, bars = [], []
    col = 0
    while col < len(body):
        ch = body[col]
        if ch == "|":
            bars.append(col)
            col += 1
        elif drums:
            if ch not in "-| .:":
                events.append((col, 1))
            col += 1
        elif ch.isdigit():
            j = col
            while j < len(body) and body[j].isdigit() and j - col < 2:
                j += 1
            events.append((col, int(body[col:j])))
            col = j
        else:
            col += 1
    return events, bars


def parse(text: str, drums: bool | None = None) -> ParsedTab:
    found = systems(text)
    if drums is None:
        drums = any(_looks_like_drums(s.labels) for s in found)
    tab = ParsedTab(drums=drums)
    beat0 = 0.0
    for system in found:
        if drums:
            pieces = [_drum_note(l) for l in system.labels]
        else:
            pieces = _string_pitches(system.labels)
        width = max(len(b) for b in system.bodies)
        bars = sorted({c for b in system.bodies for c in _events(b, drums)[1]} | {0, width})
        bar_spans = [(a, b) for a, b in zip(bars, bars[1:]) if b - a > 2]
        if not bar_spans:
            continue
        # Where each bar's music starts: right after its bar line, less the padding dashes
        # every bar of the system starts with ("|--7--5--|": 2). Notes are then spread over
        # 4 beats and snapped to 16ths (text tabs have no real rhythm).
        found: list[tuple[int, int, int, int]] = []  # (bar, column in the bar's content, its width, pitch)
        for body, piece in zip(system.bodies, pieces):
            if piece is None:
                continue
            for col, value in _events(body, drums)[0]:
                k = next((i for i, (a, b) in enumerate(bar_spans) if a <= col < b), None)
                if k is None:
                    continue
                a, b = bar_spans[k]
                start = a + (1 if body[a:a + 1] == "|" else 0)  # after the bar line
                found.append((k, col - start, b - start, piece if drums else piece + value))
        lead = min((c for _, c, _, _ in found), default=0)
        hits: list[tuple[float, int]] = []
        for k, c, width, pitch in found:
            content = max(1, width - lead)
            pos = min(3.75, round((c - lead) / content * 4 * 4) / 4)
            hits.append((k * 4 + pos, pitch))
        length = 4.0 * len(bar_spans)
        for r in range(system.repeat):
            starts = sorted({b for b, _ in hits})
            for beat, pitch in hits:
                later = [s for s in starts if s > beat]
                dur = min((later[0] if later else length) - beat, 1.0)
                tab.notes.append(TabNote(beat0 + r * length + beat, 0.1 if drums else max(dur, 0.1), pitch))
        beat0 += length * system.repeat
    tab.notes.sort(key=lambda n: (n.beat, n.pitch))
    tab.beats = beat0
    return tab


def to_midi(text: str, out: Path, instrument: str) -> int:
    """Writes a one-track MIDI score (120 BPM; the engine aligns it to the recording).
    Returns the number of notes."""
    tab = parse(text, drums=instrument == "drums" or None)
    res = 480
    midi = mido.MidiFile(type=1, ticks_per_beat=res)
    track = mido.MidiTrack()
    name = {"guitar": "Distortion Guitar", "bass": "Bass", "drums": "Drums"}.get(instrument, instrument)
    track.append(mido.MetaMessage("track_name", name=name, time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=500000, time=0))
    channel = 9 if tab.drums else 0
    if not tab.drums:
        track.append(mido.Message("program_change", program=33 if instrument == "bass" else 30, channel=channel, time=0))
    events = []
    for n in tab.notes:
        on = int(round(n.beat * res))
        events.append((on, 1, n.pitch))
        events.append((on + max(1, int(round(n.length * res))), 0, n.pitch))
    now = 0
    for tick, kind, pitch in sorted(events):
        msg = "note_on" if kind else "note_off"
        track.append(mido.Message(msg, note=max(0, min(127, pitch)), velocity=100 if kind else 0, channel=channel, time=tick - now))
        now = tick
    midi.tracks.append(track)
    midi.save(str(out))
    return len(tab.notes)
