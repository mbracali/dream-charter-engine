"""gd-tool-02: a Guitar Pro / MIDI score or chart just added to a Song home box.

    python inspect_sheet.py PROJECT_DIR BOX FILE_NAME

Lists its tracks (cached in PROJECT/sheets/.tracks/<file name>.tsv for the window: index, guess,
notes, drums, name; a "#default" line per box), then copies the file into every other box
(guitar, bass, drums) it has an instrument for, so one file fills the whole band (R32).
Prints STATUS / FILLED <boxes> / CHOOSE <box> (several guitars, no lead named) /
LENGTH <seconds of music in the file, or -1 when it can't tell> / DONE / ERROR.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import sheets


def emit(*parts: str) -> None:
    print(" ".join(parts), flush=True)


def inspect(project: Path, box: str, name: str) -> list[str]:
    source = project / "sheets" / box / name
    if sheets.kind_of(source, box) not in ("tab", "score", "chart"):
        return []
    tracks = sheets.score_tracks(source)
    seconds = sheets.music_seconds(source)
    cache = project / "sheets" / ".tracks"
    cache.mkdir(parents=True, exist_ok=True)
    lines = [f"{t['index']}\t{t['guess']}\t{t['notes']}\t{1 if t['drums'] else 0}\t{t['name']}" for t in tracks]
    lines.append(f"#length\t{seconds if seconds else -1:.1f}")
    filled, unsure = [], []
    for instrument in sheets.INSTRUMENTS:
        index, sure = sheets.default_track(tracks, instrument)
        if index is None:
            continue
        lines.append(f"#default\t{instrument}\t{index}\t{1 if sure else 0}")
        if not sure:
            unsure.append(instrument)
        target = project / "sheets" / instrument / name
        if instrument != box and not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            filled.append(instrument)
    (cache / f"{name}.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    for t in tracks:
        emit("STATUS", f"{name}: track {t['index']} {t['name']} ({t['guess']}, {t['notes']} notes)")
    emit("LENGTH", f"{seconds if seconds else -1:.1f}")
    if filled:
        emit("FILLED", ",".join(filled))
    for instrument in unsure:
        emit("CHOOSE", instrument)
    return filled


def main() -> int:
    try:
        project, box, name = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
        inspect(project, box, name)
        emit("DONE")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the window
        emit("ERROR", f"{type(exc).__name__}: {exc}".replace("\n", " "))
        return 1


if __name__ == "__main__":
    sys.exit(main())
