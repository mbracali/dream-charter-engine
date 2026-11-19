"""gd-tool-02 Song forge: the best chart we can make of a project.

    python forge.py PROJECT_DIR [--mode demucs|simple]

Reads everything from PROJECT_DIR/project.yaml and PROJECT_DIR/sheets/ (Song home):

1. Stems: separates the song first if stems/ is not complete (Demucs, offline).
2. Sheets (sheets.py): per instrument, a Guitar Pro / MIDI score guides the engine's
   transcription; a chord sheet limits the chords (and can give the lyrics); an
   existing chart is used as-is for its part. The lyrics box gives the vocals.
3. dream-charter-engine (ai_charter) charts what is left from the stems.
4. Writes forge/<Artist - Title>/notes.mid (the engine's, kept for the tempo map),
   forge/chart.txt (what Fine tuning edits and Export reads, see chart_text.py) and
   forge/forged.txt (a copy, for "Revert to the forged chart").

project.yaml [forge]: genre, drums_style (heard/steady/locked), riffs_style
(heard/steady), keys (on/off), <part>_source (the stem a part is charted from).

Prints, one per line: STATUS <text>, PROGRESS <0..1>, STEM <name> <path>,
STEMS_DONE <mode>, COUNT <part> <easy> <medium> <hard> <expert>, DONE, ERROR <text>.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

# The engine's best parts, offline: Beat This! (beats and bar starts) reads its checkpoint
# from TORCH_HOME/hub/checkpoints/, kept in gd-tool-02/models/torch; ADTOF's weights ship
# inside its package. Nothing is downloaded at run time.
os.environ.setdefault("TORCH_HOME", str(Path(__file__).resolve().parent.parent / "models" / "torch"))
os.environ["HF_HUB_OFFLINE"] = "1"

import numpy as np
import soundfile as sf
import yaml

import chart_text
import sheets
from midi_io import midi_to_chart

STEM_NAMES = ("drums", "bass", "other", "vocals", "guitar", "keys")
DRUM_SIMILARITY = {"heard": 0.9, "steady": 0.8, "locked": 0.7}
RIFF_SIMILARITY = {"heard": 0.0, "steady": 0.8}


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


class Progress:
    """PROGRESS lines for one stretch [lo, hi] of the whole job."""

    def __init__(self) -> None:
        self.lo, self.hi, self.last = 0.0, 1.0, -1.0

    def stage(self, lo: float, hi: float) -> None:
        self.lo, self.hi = lo, hi
        self(0.0)

    def __call__(self, fraction: float) -> None:
        value = self.lo + (self.hi - self.lo) * max(0.0, min(1.0, fraction))
        if value - self.last >= 0.002 or fraction >= 1.0:
            self.last = value
            emit("PROGRESS", f"{value:.3f}")


# ---- the engine, without ffmpeg ------------------------------------------------

def patch_engine_audio() -> None:
    from ai_charter.export import audio

    def to_wav(src: Path, dst: Path, sample_rate: int = 44100) -> None:
        data, sr = sf.read(str(src), dtype="float32", always_2d=True)
        if sr != sample_rate:
            from math import gcd
            from scipy.signal import resample_poly
            g = gcd(sr, sample_rate)
            data = resample_poly(data, sample_rate // g, sr // g, axis=0).astype(np.float32)
        sf.write(str(dst), data[:, :2], sample_rate, subtype="PCM_16")

    audio.require_ffmpeg = lambda: None
    audio.read_tags = lambda src: {}
    audio.to_wav = to_wav
    audio.to_ogg = lambda *a, **k: None
    audio.mix_to_ogg = lambda *a, **k: None
    audio.extract_cover = lambda *a, **k: False


class OfflineError(RuntimeError):
    pass


def block_downloads() -> None:
    """Everything runs offline: a model that is not in gd-tool-02/models is not fetched.
    torch.hub (MMS_FA lyric alignment, Beat This!) and plain urllib (Whisper) raise
    instead, and the engine falls back to what it has (the older lyric aligner)."""
    import urllib.request

    def refuse(url, *args, **kwargs):
        raise OfflineError(f"offline: not downloading {url} (put the model in gd-tool-02/models, see README)")

    urllib.request.urlopen = refuse
    try:
        import torch.hub

        torch.hub.download_url_to_file = refuse
    except ImportError:
        pass


SCORE_ONLY = ("guitar", "bass")  # with a sheet, these parts are the sheet's notes only
GUIDED_PARTS: set[str] = set()   # parts that really got a guide this run


class ScoreOnly:
    """Wraps the engine's Guide so a guitar / bass with a tab is the tab's notes only (R19):
    the engine's "seguir" already takes the score's notes where it has them; this also drops
    what the engine heard before / after the part the score covers, and charts nothing when
    the score doesn't match the recording (instead of falling back to the audio)."""

    def __init__(self, guide) -> None:
        self.guide = guide

    def __call__(self, cands: list, source: str = "", log=print) -> list:
        out = self.guide(cands, source, log)
        name = self.guide.instrument
        if not self.guide.stats.get("applied"):
            say(f"{name}: the sheet doesn't match the recording, so the {name} is not charted")
            return []
        span = self.guide.span
        if span:
            out = [c for c in out if span[0] <= c.tick <= span[1]]
        say(f"{name}: {len({c.tick for c in out})} notes from the sheet")
        return out

    def __getattr__(self, attr):
        return getattr(self.guide, attr)


def install_references(refs: dict[str, Path], fidelity_of: dict[str, str] | None = None) -> None:
    """The engine takes one score with a track -> instrument map. Song home gives one file
    per instrument, so its load_guides is replaced by one that loads each file for its own
    instrument and merges the guides. `fidelity`: per instrument (text tabs: "corrigir")."""
    import ai_charter.reference as reference

    original = reference.load_guides

    def load_all(_path, _mapping, y, sr, tempo, rules, log=print, fidelity=reference.DEFAULT_FIDELITY,
                 sections=None, anchors=None):
        # One call per file: a file used for several instruments (a full Guitar Pro score in
        # the Guitar and Drums boxes) is aligned once, its pitched tracks carrying the drums.
        by_file: dict[Path, list[str]] = {}
        for instrument, file in refs.items():
            by_file.setdefault(file, []).append(instrument)
        guides = {}
        for file, instruments in by_file.items():
            try:  # one bad file must not stop the forge: it is skipped, the stems are charted alone
                mapping: dict[int, str] = {}
                for instrument in instruments:
                    found_tracks = sheets.pick_tracks(file, instrument)
                    if not found_tracks:
                        say(f"{file.name}: no track in it fits the {instrument}")
                    mapping.update(found_tracks)
                if not mapping:
                    continue
                levels = {i: (fidelity_of or {}).get(i, fidelity) for i in reference.GUIDED}
                found = original(file, mapping, y, sr, tempo, rules, log, levels)
            except Exception as exc:  # noqa: BLE001
                say(f"{file.name}: can't be read ({english_error(exc)})")
                log(f"{file.name}: {exc}")
                continue
            for instrument in instruments:
                if instrument in found:
                    guide = found[instrument]
                    guides[instrument] = ScoreOnly(guide) if instrument in SCORE_ONLY else guide
                    GUIDED_PARTS.add(instrument)
                    say(f"{file.name} drives the {instrument}")
        return guides

    reference.load_guides = load_all


def write_rules(folder: Path, drum_similarity: float, riff_similarity: float | dict[str, float] = 0.0) -> Path:
    """Our rules on top of the engine's defaults and the genre preset (load_rules merges
    defaults -> genre -> this file). drum_similarity: bars at least this alike get the same
    drum notes (the engine's repeat_similarity; it ships 0.9, lower = steadier grooves).
    riff_similarity: the same for guitar, bass and keys (0 = off, the engine's default); one
    value, or one per part (a part driven by a sheet gets 0: the sheet's notes stay as written)."""
    path = folder / "forge_rules.toml"
    text = f"# gd-tool-02 Forge\n[drum_detection]\nrepeat_similarity = {drum_similarity:g}\n"
    for part in ("guitar", "bass", "keys"):
        value = riff_similarity.get(part, 0.0) if isinstance(riff_similarity, dict) else riff_similarity
        text += f"\n[detection.{part}]\nrepeat_similarity = {value:g}\n"
    path.write_text(text, encoding="utf-8")
    return path


# ---- English for the window ------------------------------------------------------------
# The engine (ai_charter) talks Portuguese. Everything it says goes to forge/forge.log; the
# window gets the lines below in English, and nothing else (R18).

LOG_FILE: Path | None = None


def say(text: str) -> None:
    """One of our own (English) lines: shown and logged."""
    write_log(text)
    emit("STATUS", text)


def write_log(text: str) -> None:
    if LOG_FILE is not None:
        with LOG_FILE.open("a", encoding="utf-8") as out:
            out.write(text.rstrip() + "\n")


STEPS = {"1": "Step 1 of 5: reading the song...", "2": "Step 2 of 5: finding the beat and the bars...",
         "3": "Step 3 of 5: loading the stems...", "4": "Step 4 of 5: charting every part...",
         "5": "Step 5 of 5: writing the chart..."}
PARTS_PT = r"(guitar|bass|keys|drums|vocals)"
TRANSLATIONS = [
    (r"^(\d)/5\b", lambda m: STEPS.get(m[1])),
    (r"^BPM médio: ([\d.]+)", lambda m: f"Average tempo: {m[1]} BPM"),
    (r"^Referência (.+?):", lambda m: f"Reading the sheet {m[1]}..."),
    (r"^alinhada com a gravação \(distância ([\d.]+)", lambda m: f"Sheet aligned to the recording (distance {m[1]})"),
    (rf"^{PARTS_PT}.*?: a referência confere com só (\d+)% das (notas detectadas|batidas)",
     lambda m: f"{m[1]}: the sheet matches only {m[2]}% of what is heard"),
    (rf"^{PARTS_PT}.*?: referência confere com (\d+)% das (notas detectadas|batidas)",
     lambda m: f"{m[1]}: the sheet matches {m[2]}% of what is heard"),
    (rf"^{PARTS_PT}: sem stem", lambda m: f"{m[1]}: no stem, skipped"),
    (rf"^{PARTS_PT}: stem quase silencioso", lambda m: f"{m[1]}: the stem is almost silent, skipped"),
    (rf"^{PARTS_PT}: usando o stem (\w+)", lambda m: f"{m[1]}: charted from the {m[2]} stem"),
    (rf"^{PARTS_PT}: expert (\d+)", lambda m: f"{m[1]}: {m[2]} Expert notes"),
    (r"^vocals: (\d+) notas em (\d+) frases", lambda m: f"vocals: {m[1]} notes in {m[2]} phrases"),
    (r"^vocals: sem letra", lambda m: "vocals: no lyrics, skipped"),
    (r"^vocals: letra tirada da cifra", lambda m: "vocals: lyrics taken from the chord sheet"),
    (r"^vocals: alinhamento forçado indisponível", lambda m: "vocals: placing the lyrics with the simpler aligner"),
    (r"^vocals: sem stem", lambda m: "vocals: no vocal stem, skipped"),
]


def translate(text: str) -> str | None:
    """The engine's line in English, or None (it stays in forge.log only)."""
    text = text.strip()
    for pattern, english in TRANSLATIONS:
        m = re.search(pattern, text)
        if m:
            return english(m)
    return None


def english_error(exc: BaseException) -> str:
    """An error for the window: ours are English; the engine's become a pointer to the log."""
    message = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
    portuguese = re.search(r"[ãõçáéíóúâêô]| não | com | para | da | do | pista| partitura", message, re.I)
    return "the engine stopped; details in forge/forge.log" if portuguese else message


# ---- the plan: what the sheets say ---------------------------------------------------

def text_tab_score(project: Path, box: str, file: Path) -> Path:
    """A pasted text tab as a MIDI score in forge/tabs/ (the engine reads MIDI scores)."""
    import ascii_tab

    out = project / "forge" / "tabs" / f"{box} - {file.stem}.mid"
    out.parent.mkdir(parents=True, exist_ok=True)
    count = ascii_tab.to_midi(file.read_text(encoding="utf-8", errors="replace"), out, box)
    emit("STATUS", f"{file.name}: text tab, {count} notes")
    return out


class Plan:
    def __init__(self, project: Path, doc: dict) -> None:
        forge = doc.get("forge") or {}
        self.genre = str(forge.get("genre") or "") or None
        self.drum_similarity = DRUM_SIMILARITY.get(str(forge.get("drums_style") or "steady"), 0.8)
        self.riff_similarity = RIFF_SIMILARITY.get(str(forge.get("riffs_style") or "steady"), 0.8)
        self.sources = {p: str(forge.get(f"{p}_source")) for p in ("guitar", "bass", "keys")
                        if forge.get(f"{p}_source") and str(forge.get(f"{p}_source")) != p}
        parts = ["guitar", "bass", "drums"] + (["keys"] if str(forge.get("keys") or "") == "on" else [])
        sheets_doc = doc.get("sheets") or {}
        self.refs: dict[str, Path] = {}
        self.fidelity: dict[str, str] = {}
        self.charts: dict[str, Path] = {}
        self.chords: Path | None = None
        chord_boxes: set[str] = set()
        self.notes: list[str] = []
        for box in sheets.INSTRUMENTS:
            for file in sheets.checked(project, doc, box):
                kind = sheets.kind_of(file, box)
                if kind == "chart" and box not in self.charts:
                    self.charts[box] = file
                elif kind in ("tab", "score", "texttab"):
                    if box in self.refs:
                        self.notes.append(f"{file.name}: one tab per instrument for now, using {self.refs[box].name}")
                    else:
                        self.refs[box] = text_tab_score(project, box, file) if kind == "texttab" else file
                        self.fidelity[box] = "seguir"  # the tab's notes, not the heard ones
                elif kind == "chords":
                    chord_boxes.add(box)
                    self.chords = self.chords or file
        # Guitar and bass need a sheet (R20): a chart, a tab, or at least a chord sheet; or the
        # user says the song has none ("This song has no guitar").
        self.skipped: list[str] = []
        for part in ("guitar", "bass"):
            has_sheet = part in self.charts or part in self.refs or part in chord_boxes
            if str(sheets_doc.get(f"{part}_absent") or "") == "yes":
                parts.remove(part)
                self.charts.pop(part, None)
                self.refs.pop(part, None)
                self.skipped.append(part)
            elif not has_sheet:
                raise ValueError(f"the {part} needs a sheet (a tab, MIDI, chart or chord sheet), "
                                 f"or tick \"This song has no {part}\" on Song home")
        self.parts = parts
        # An attached chart replaces the engine for its part.
        self.engine_parts = [p for p in parts if p not in self.charts]
        # Riff steadiness only for parts the engine hears; sheet-driven parts stay as written.
        self.riff_by_part = {p: 0.0 if p in self.refs else self.riff_similarity for p in ("guitar", "bass", "keys")}
        self.lyrics, self.use_cifra_lyrics = self._lyrics(project, doc)

    @staticmethod
    def _lyrics(project: Path, doc: dict) -> tuple[Path | None, bool]:
        use = str((doc.get("sheets") or {}).get("lyrics_use") or "")
        if use == "none":
            return None, False
        typed = project / "lyrics.txt"
        has_typed = typed.exists() and typed.read_text(encoding="utf-8", errors="replace").strip()
        files = sheets.checked(project, {"sheets": {"lyrics_use": ""}}, "lyrics")
        chosen: Path | None = None
        if use in ("", "typed") and has_typed:
            chosen = typed
        elif use and use != "typed":
            chosen = next((f for f in files if f.name == use), None)
        elif files:
            chosen = files[0]
        if chosen is None:
            return None, True  # a chord sheet may still give them
        if chosen != typed:
            out = project / "forge" / "lyrics_used.txt"
            out.parent.mkdir(exist_ok=True)
            out.write_text(sheets.lyrics_text(chosen) + "\n", encoding="utf-8")
            return out, True
        return typed, True


# ---- the forge ---------------------------------------------------------------------------

def stems_complete(stems: Path) -> bool:
    return (stems / ".done").exists() and any((stems / f"{n}.wav").exists() for n in ("drums", "guitar", "bass"))


def ensure_stems(project: Path, song: Path, mode: str, progress: Progress) -> None:
    stems = project / "stems"
    if stems_complete(stems):
        for wav in sorted(stems.glob("*.wav")):
            emit("STEM", wav.stem, str(wav))
        return
    import separate_stems

    emit("STATUS", "Separating the stems (Demucs)..." if mode == "demucs" else "Splitting the stems...")
    separate_stems.separate(song, stems, mode, progress=progress)
    emit("STEMS_DONE", mode)


def run_engine(project: Path, plan: Plan, progress: Progress) -> Path:
    block_downloads()
    patch_engine_audio()
    if plan.refs:
        install_references(plan.refs, plan.fidelity)
    from ai_charter.pipeline import Options, run

    doc = yaml.safe_load((project / "project.yaml").read_text(encoding="utf-8")) or {}
    song = project / str((doc.get("project") or {}).get("audio_file") or "")
    song_info = doc.get("song") or {}
    out = project / "forge"
    keep = {"lyrics_used.txt", "tabs", "forge.log"}
    for item in list(out.iterdir()) if out.exists() else []:
        if item.name not in keep:
            shutil.rmtree(item) if item.is_dir() else item.unlink()
    out.mkdir(exist_ok=True)
    opts = Options(
        rules_path=write_rules(out, plan.drum_similarity, plan.riff_by_part),
        input=song, output_dir=out, separator="folder", stems_dir=project / "stems",
        genre=plan.genre, lead_in=0.0,
        instruments=tuple(plan.engine_parts),
        stem_sources=plan.sources or None,
        title=str(song_info.get("title") or project.name),
        artist=str(song_info.get("artist") or "Unknown Artist"),
        lyrics=plan.lyrics,
        cifra=plan.chords, cifra_lyrics=plan.use_cifra_lyrics,
        reference=next(iter(plan.refs.values()), None),
    )

    def log(message) -> None:
        text = str(message).strip()
        write_log(text)
        step = re.match(r"^(\d)/(\d)\b", text)
        if step:
            progress(int(step[1]) / (int(step[2]) + 1))
        english = translate(text)
        if english:
            emit("STATUS", english)

    result = run(opts, log=log)
    return result.song_dir / "notes.mid"


def song_duration(project: Path) -> float:
    duration = 0.0
    for wav in (project / "stems").glob("*.wav"):
        duration = max(duration, sf.info(str(wav)).duration)
    return duration


def build_chart(project: Path, notes_mid: Path, plan: Plan) -> chart_text.Chart:
    chart = midi_to_chart(notes_mid, song_duration(project))
    # A guitar / bass whose sheet couldn't be used is not charted from the audio instead.
    for part, file in plan.refs.items():
        if part in SCORE_ONLY and part not in GUIDED_PARTS:
            chart.replace_part(part, [])
            say(f"{part}: {file.name} couldn't be lined up with the recording, so the {part} is not charted")
    for part, file in plan.charts.items():
        notes = sheets.chart_part(file, part)
        if notes:
            chart.replace_part(part, notes)
            emit("STATUS", f"{part}: using the chart {file.name} as-is")
        else:
            emit("STATUS", f"{file.name} has no {part} part")
    return chart


def write_preview(notes_mid: Path, out: Path, duration: float) -> dict[str, list[int]]:
    """notes.mid -> chart text (kept for the tests and --preview-only)."""
    chart = midi_to_chart(notes_mid, duration)
    chart_text.write(chart, out)
    return counts(chart)


def counts(chart: chart_text.Chart) -> dict[str, list[int]]:
    out = {part: [chart.chord_count(part, d) for d in range(4)] for part in chart.parts()}
    if chart.vocals:
        out["vocals"] = [len(chart.vocals)] * 4
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--mode", choices=("demucs", "simple"), default="demucs")
    parser.add_argument("--preview-only", type=Path, help="debug: convert this notes.mid only")
    args = parser.parse_args()
    progress = Progress()
    try:
        project = args.project
        if args.preview_only:
            write_preview(args.preview_only, project / "forge" / "chart.txt", song_duration(project))
            emit("DONE")
            return 0
        doc = yaml.safe_load((project / "project.yaml").read_text(encoding="utf-8")) or {}
        song = project / str((doc.get("project") or {}).get("audio_file") or "")
        if not song.is_file():
            raise FileNotFoundError(f"the project has no song file ({song.name or 'audio_file is empty'})")
        progress(0.0)
        separated = stems_complete(project / "stems")
        progress.stage(0.0, 0.0 if separated else 0.55)
        ensure_stems(project, song, args.mode, progress)
        progress.stage(0.0 if separated else 0.55, 0.95)
        (project / "forge").mkdir(exist_ok=True)
        global LOG_FILE
        LOG_FILE = project / "forge" / "forge.log"
        LOG_FILE.write_text("", encoding="utf-8")
        plan = Plan(project, doc)
        for part in plan.skipped:
            say(f"{part}: not in this song, skipped")
        for note in plan.notes:
            emit("STATUS", note)
        used = [f"{p}: {f.name}" for p, f in {**plan.refs, **plan.charts}.items()]
        emit("STATUS", "Charting with " + (", ".join(used) if used else "the audio only") + "...")
        notes_mid = run_engine(project, plan, progress)
        progress.stage(0.95, 1.0)
        emit("STATUS", "Preparing the board...")
        chart = build_chart(project, notes_mid, plan)
        chart_text.write(chart, project / "forge" / "chart.txt")
        shutil.copyfile(project / "forge" / "chart.txt", project / "forge" / "forged.txt")
        for part, c in counts(chart).items():
            emit("COUNT", part, *map(str, c))
        progress(1.0)
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        import traceback

        write_log(traceback.format_exc())
        emit("ERROR", english_error(exc).replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
