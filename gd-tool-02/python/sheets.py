"""The music sheets a project has (Song home): what each file is and how Forge uses it.

    PROJECT/sheets/<instrument>/<file>     instrument: guitar, bass, drums, lyrics
    project.yaml  sheets: <instrument>_use  "a.gp5|b.txt" = the checked files
                                            (empty = all; "none" = none)

Kinds (by extension, and for MIDI by content):
    tab     Guitar Pro (.gp3 .gp4 .gp5 .gpx .gp .gp6 .gp7 .gp8): a score that guides the
            engine's transcription (pitches, chords, rhythm).
    score   MIDI without game tracks: the same, as a score.
    chart   An existing chart (.chart, or a MIDI with PART tracks): used as-is.
    chords  A chord sheet (.txt, Cifra Club / Ultimate Guitar): limits the chords the
            engine recognises and can give the lyrics.
    texttab A text (ASCII) tab (.txt / .tab: "e|--0--3--|" lines, or drum lines like
            "HH|x-x-x-|"): converted to a MIDI score (ascii_tab.py) that corrects the
            pitches and chords the engine hears (not its rhythm).
    lyrics  In the lyrics box: .txt / .lrc (LRC timestamps are dropped).
"""

from __future__ import annotations

import re
from pathlib import Path

from chart_text import Chart, Note
from midi_io import midi_parts, midi_to_chart

INSTRUMENTS = ("guitar", "bass", "drums")
TAB_EXT = {".gp3", ".gp4", ".gp5", ".gpx", ".gp", ".gp6", ".gp7", ".gp8"}
MIDI_EXT = {".mid", ".midi", ".kar"}


def kind_of(path: Path, box: str) -> str | None:
    ext = path.suffix.lower()
    if box == "lyrics":
        return "lyrics" if ext in (".txt", ".lrc") else None
    if ext in TAB_EXT:
        return "tab"
    if ext in MIDI_EXT:
        return "chart" if midi_parts(path) - {"vocals"} else "score"
    if ext == ".chart":
        return "chart"
    if ext in (".txt", ".tab"):
        from ascii_tab import is_ascii_tab

        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None
        return "texttab" if is_ascii_tab(text) else ("chords" if ext == ".txt" else None)
    return None


def checked(project: Path, doc: dict, box: str) -> list[Path]:
    """The files of a box the user left checked, in name order."""
    folder = project / "sheets" / box
    files = sorted(p for p in folder.glob("*") if p.is_file() and not p.name.startswith(".")) if folder.is_dir() else []
    use = str(((doc.get("sheets") or {}).get(f"{box}_use")) or "")
    if use == "none":
        return []
    if use:
        names = set(use.split("|"))
        files = [f for f in files if f.name in names]
    return [f for f in files if kind_of(f, box)]


def lyrics_text(path: Path) -> str:
    """A .txt or .lrc as one phrase per line (LRC tags and timestamps removed)."""
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if path.suffix.lower() == ".lrc":
            if re.match(r"^\[[a-z]+:.*\]$", line.strip(), re.I):  # [ar: ...] [ti: ...]
                continue
            line = re.sub(r"\[\d+:\d+(?:[.:]\d+)?\]", "", line)
            line = re.sub(r"<\d+:\d+(?:[.:]\d+)?>", "", line)
        out.append(line.strip())
    text = "\n".join(out).strip()
    return re.sub(r"\n{3,}", "\n\n", text)


# ---- references: which score track guides which instrument -------------------------

def pick_tracks(path: Path, instrument: str) -> dict[int, str]:
    """{track index: instrument} for the tracks of a tab/score that fit `instrument`:
    the engine's own guess first, then the best candidate (drum tracks for drums; for
    bass the lowest pitched track, for guitar the busiest non-bass one)."""
    from ai_charter.reference import default_mapping, read_score

    score = read_score(path)
    guess = {i: inst for i, inst in default_mapping(score).items() if inst == instrument}
    if guess:
        return guess
    tracks = [t for t in score.tracks if t.notes]
    if instrument == "drums":
        drums = [t for t in tracks if getattr(t, "drums", False)]
        return {drums[0].index: "drums"} if drums else {}
    pitched = [t for t in tracks if not getattr(t, "drums", False)]
    if not pitched:
        return {}

    def mean_pitch(t) -> float:
        return sum(n.pitch for n in t.notes) / len(t.notes)

    if instrument == "bass":
        best = min(pitched, key=mean_pitch)
    else:
        candidates = [t for t in pitched if t.guess != "bass"] or pitched
        best = max(candidates, key=lambda t: len(t.notes))
    return {best.index: instrument}


# ---- existing charts ----------------------------------------------------------------

CHART_SECTIONS = {"Single": "guitar", "DoubleGuitar": "guitar", "DoubleBass": "bass", "DoubleRhythm": "bass",
                  "Keyboard": "keys", "Drums": "drums"}
CHART_DIFFS = {"Easy": 0, "Medium": 1, "Hard": 2, "Expert": 3}


def read_chart_file(path: Path) -> Chart:
    """A Moonscraper / Clone Hero .chart as a Chart (seconds from its own SyncTrack;
    the song's Offset is ignored: the chart is taken as made for this audio)."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    sections: dict[str, list[str]] = {}
    for name, body in re.findall(r"\[([^\]]+)\]\s*\{(.*?)\}", text, re.S):
        sections[name.strip()] = [l.strip() for l in body.splitlines() if l.strip()]
    res = 192
    for line in sections.get("Song", []):
        key, _, value = line.partition("=")
        if key.strip() == "Resolution":
            res = int(value.strip())
    bpms: list[tuple[int, float]] = []
    meters: list[tuple[int, int]] = []
    for line in sections.get("SyncTrack", []):
        m = re.match(r"(\d+)\s*=\s*(B|TS)\s+(\d+)", line)
        if m:
            tick, kind, value = int(m[1]), m[2], int(m[3])
            if kind == "B":
                bpms.append((tick, value / 1000.0))
            else:
                meters.append((tick, value))
    bpms.sort()
    if not bpms or bpms[0][0] != 0:
        bpms.insert(0, (0, 120.0))

    def seconds(tick: int) -> float:
        t, last_tick, bpm = 0.0, 0, bpms[0][1]
        for change_tick, change_bpm in bpms:
            if change_tick > tick:
                break
            t += (change_tick - last_tick) / res * 60.0 / bpm
            last_tick, bpm = change_tick, change_bpm
        return t + (tick - last_tick) / res * 60.0 / bpm

    chart = Chart()
    end = 0
    meters = sorted(meters) or [(0, 4)]
    for line in sections.get("Events", []):
        m = re.match(r'(\d+)\s*=\s*E\s+"section (.*)"', line)
        if m:
            chart.sections.append((seconds(int(m[1])), m[2].strip()))
    for name, lines in sections.items():
        diff = next((d for prefix, d in CHART_DIFFS.items() if name.startswith(prefix)), None)
        part = CHART_SECTIONS.get(name[len(next((p for p in CHART_DIFFS if name.startswith(p)), "")):])
        if diff is None or part is None or any(n.part == part and n.diff == diff for n in chart.notes):
            continue
        drums = part == "drums"
        notes: dict[int, list[tuple[int, int]]] = {}
        flags: dict[int, set[int]] = {}
        stars: list[tuple[int, int]] = []
        for line in lines:
            m = re.match(r"(\d+)\s*=\s*([NS])\s+(\d+)\s+(\d+)", line)
            if not m:
                continue
            tick, kind, value, length = int(m[1]), m[2], int(m[3]), int(m[4])
            end = max(end, tick + length)
            if kind == "S" and value == 2:
                stars.append((tick, tick + max(1, length)))
            elif kind == "N":
                if drums:
                    if value in (66, 67, 68):
                        flags.setdefault(tick, set()).add(value)
                    elif value <= 5:
                        lane = 4 if value == 5 else value  # 5-lane green -> our green
                        notes.setdefault(tick, []).append((lane, length))
                elif value <= 4:
                    notes.setdefault(tick, []).append((value, length))
                elif value in (5, 6):
                    flags.setdefault(tick, set()).add(value)
        prev_tick, prev_lanes = None, None
        for tick in sorted(notes):
            group = notes[tick]
            lanes = sorted(l for l, _ in group)
            hopo = False
            if not drums:
                natural = (prev_tick is not None and tick - prev_tick <= res // 3 + 1 and len(lanes) == 1
                           and lanes != prev_lanes)
                forced = 5 in flags.get(tick, set())
                hopo = natural != forced or 6 in flags.get(tick, set())
            star = any(a <= tick < b for a, b in stars)
            for lane, length in group:
                f = ("h" if hopo else "") + ("s" if star else "")
                if drums and lane >= 2 and (64 + lane) in flags.get(tick, set()):
                    f += "c"
                start = seconds(tick)
                sustain = seconds(tick + length) - start if length > res // 3 and not drums else 0.0
                chart.notes.append(Note(part, diff, start, sustain, lane, f))
            prev_tick, prev_lanes = tick, lanes
    chart.duration = seconds(end) if end else 0.0
    return chart


def chart_part(path: Path, part: str) -> list[Note]:
    """The notes of `part` in an existing chart (.chart or a notes.mid with PART tracks)."""
    chart = read_chart_file(path) if path.suffix.lower() == ".chart" else midi_to_chart(path)
    return chart.part(part)
