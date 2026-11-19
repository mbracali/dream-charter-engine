"""The project's chart in plain text: forge/chart.txt.

Forge writes it (from the engine's notes.mid and any chart the user attached as-is),
Fine tuning (the C++ editor) rewrites it, Export turns it back into notes.mid. One
event per line, times in seconds of the project's audio (no lead-in):

    SONG <duration>
    BEAT <t> <1 = first beat of a bar, else 0>
    SECTION <t> <name>
    NOTE <part> <difficulty 0-3> <t> <sustain> <lane> <flags>
        part: guitar / bass / keys / drums; difficulty: 0 Easy .. 3 Expert
        lane: frets 0 green .. 4 orange; drums 0 kick, 1 red, 2 yellow, 3 blue, 4 green
        flags: h HOPO, s Star Power, c cymbal (drums), - none
    VOCAL <t> <duration> <pitch> <lyric>
    PHRASE <t> <end>

Notes at the same time in the same part and difficulty are a chord.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

PARTS = ("guitar", "bass", "keys", "drums")


@dataclass
class Note:
    part: str
    diff: int
    t: float
    sustain: float
    lane: int
    flags: str = ""

    @property
    def hopo(self) -> bool:
        return "h" in self.flags

    @property
    def star(self) -> bool:
        return "s" in self.flags

    @property
    def cymbal(self) -> bool:
        return "c" in self.flags


@dataclass
class Vocal:
    t: float
    dur: float
    pitch: int
    lyric: str = ""


@dataclass
class Chart:
    duration: float = 0.0
    beats: list[tuple[float, bool]] = field(default_factory=list)
    sections: list[tuple[float, str]] = field(default_factory=list)
    notes: list[Note] = field(default_factory=list)
    vocals: list[Vocal] = field(default_factory=list)
    phrases: list[tuple[float, float]] = field(default_factory=list)

    def part(self, part: str, diff: int | None = None) -> list[Note]:
        return [n for n in self.notes if n.part == part and (diff is None or n.diff == diff)]

    def parts(self) -> list[str]:
        return [p for p in PARTS if any(n.part == p for n in self.notes)]

    def replace_part(self, part: str, notes: list[Note]) -> None:
        self.notes = [n for n in self.notes if n.part != part] + [Note(part, n.diff, n.t, n.sustain, n.lane, n.flags)
                                                                   for n in notes]

    def chord_count(self, part: str, diff: int) -> int:
        return len({round(n.t, 4) for n in self.part(part, diff)})


def read(path: Path) -> Chart:
    chart = Chart()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        kind, _, rest = line.partition(" ")
        try:
            if kind == "SONG":
                chart.duration = float(rest)
            elif kind == "BEAT":
                t, bar = rest.split()
                chart.beats.append((float(t), bar == "1"))
            elif kind == "SECTION":
                t, _, name = rest.partition(" ")
                chart.sections.append((float(t), name))
            elif kind == "NOTE":
                part, diff, t, sus, lane, flags = rest.split()
                chart.notes.append(Note(part, int(diff), float(t), float(sus), int(lane), "" if flags == "-" else flags))
            elif kind == "VOCAL":
                bits = rest.split(" ", 3)
                chart.vocals.append(Vocal(float(bits[0]), float(bits[1]), int(bits[2]), bits[3] if len(bits) > 3 else ""))
            elif kind == "PHRASE":
                a, b = rest.split()
                chart.phrases.append((float(a), float(b)))
        except ValueError:
            continue  # a broken line (hand edit) is skipped, not fatal
    return chart


def write(chart: Chart, path: Path) -> None:
    lines = [f"SONG {chart.duration:.3f}"]
    lines += [f"BEAT {t:.4f} {1 if bar else 0}" for t, bar in chart.beats]
    lines += [f"SECTION {t:.4f} {name}" for t, name in chart.sections]
    for n in sorted(chart.notes, key=lambda n: (PARTS.index(n.part) if n.part in PARTS else 9, n.diff, n.t, n.lane)):
        lines.append(f"NOTE {n.part} {n.diff} {n.t:.4f} {n.sustain:.4f} {n.lane} {n.flags or '-'}")
    for v in sorted(chart.vocals, key=lambda v: v.t):
        lyric = v.lyric.replace("\n", " ")
        lines.append(f"VOCAL {v.t:.4f} {v.dur:.4f} {v.pitch} {lyric}".rstrip())
    lines += [f"PHRASE {a:.4f} {b:.4f}" for a, b in sorted(chart.phrases)]
    tmp = Path(str(path) + ".tmp")
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    tmp.replace(path)
