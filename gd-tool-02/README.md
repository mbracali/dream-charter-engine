# gd-tool-02: Stem Viewer (spike)

Each song is a **project**: a folder in your projects folder (chosen the first time the
app starts; change it in **Settings > Projects Folder**). Drop an MP3/FLAC/WAV on the
window and the app:

1. creates `<projects folder>/<song>/` with a copy of the song and a `project.yaml`;
2. reads everything it can from the file (`python/extract_metadata.py`): tags, cover,
   embedded lyrics, format, duration, bitrate, BPM, key, loudness; and opens the
   **Metadata** tab so you can check and fix it;
3. separates the stems in the background (`python/separate_stems.py`, Demucs
   `htdemucs_6s`, offline) into `stems/`.

A project folder:

```
Artist - Song/
  project.yaml   metadata (project, song, analysis, audio, stems)
  song.mp3       the copied song
  cover.png      cover art from the tags (if any)
  lyrics.txt     one sung phrase per line (from the tags, or typed in the app)
  stems/         mix, guitar, bass, drums, keys, vocals, other (.wav)
```

A project is opened on **Info**; its other sections are **Metadata**, **Stems** (playback,
mute/solo and separation controls), **Lyrics**, **Forge** and **Export** (not yet).

### Forge

Forge charts the project with dream-charter-engine (`python/forge.py`, which runs
`ai_charter` in-process on the project's stems, no new separation) and shows the result on
a Guitar Hero-style board that scrolls with the playback:

- **Parts**: turn Guitar, Bass, Drums, Keys and Vocals on or off, and choose the stem each
  is made from (guitar, bass and keys can take another stem, e.g. keys from `other`;
  drums are always the drums stem; vocals need the lyrics);
- **Rules**: the engine's genre presets (`ai_charter/genres/`);
- **Drums**: how steady the grooves are. The engine charts each bar from what it hears, so
  a repeating beat comes out a little different in each bar; bars at least this alike get
  the same notes: **Heard** 0.9 (the engine's default), **Steady** 0.8 (default here),
  **Locked** 0.7. Measured on a drum-machine song: 23 / 15 / 11 different bar patterns, 39% /
  67% / 74% of bars equal to the one before. Lower values can flatten a real fill;
- **Riffs**: the same for guitar, bass and keys: **Steady** (default) groups bars by rhythm
  and the notes in play, then keeps each note, and its pitch, by majority, so a riff comes out
  the same each time it repeats; **Heard** leaves each bar as detected. This is an engine rule,
  `[detection.<part>] repeat_similarity` (`consolidate_pitched` in
  `ai_charter/charting/drum_repeats.py`, off by default in the engine; Forge uses 0.8). Measured
  on a metal song: bars with an identical repeat, guitar 0% -> 78%, bass 22% -> 86%, with the
  same attacks charted; 0.7 dropped a third of the guitar's real notes, so it is not offered;
- **Listen**: the full mix, or only the stem the selected part is made from;
- **Easy / Medium / Hard / Expert**, the note count of each part, and an overview of the
  song (note density, sections) to click and seek; the mouse wheel scrubs the board.

Forge runs the engine with its best optional parts, as its README recommends: **Beat This!**
(beats and bar starts; checkpoint in `models/torch/hub/checkpoints/beat_this-final0.ckpt`,
SHA-256 `8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331`, git-ignored) and
**ADTOF** (drum transcription; weights inside its package; CC BY-NC-SA, non-commercial).
Without them the engine falls back to librosa's beat tracker (no bar starts) and rules-only
drum lanes. Tests: `.venv/bin/python -m pytest python/tests` and `ctest --test-dir build`. The app logs what it
does (jobs, stems, tabs) to `gd-tool-02.log` next to `settings.yaml`.

The charts go to `forge/<Artist - Title>/notes.mid` (+ `song.ini`), and the board reads
`forge/preview.txt`. The settings are saved in `project.yaml` (`forge:`). The game's .ogg
files are not written yet (Export), so ffmpeg is not needed.

Code: `src/app.cpp` (the window), `src/stems.*` (playback), `src/worker.*` (Python
jobs), `src/yaml_doc.*` (the YAML subset of our files), `src/text_util.h` (UTF-8), `src/forge.*` (the chart preview and the board's shapes).
Settings live in `settings.yaml` in the user's app-data folder
(`~/Library/Application Support/mbracali/gd-tool-02/` on macOS).

The window uses SDL's built-in pixel font, which only draws ASCII: accented letters
show without the accent ("Canção" -> "Cancao"), but files keep the real text.

## Setup (macOS)

```bash
brew install sdl3 cmake
python3.12 -m venv .venv && .venv/bin/pip install -r python/requirements.txt
cmake -S . -B build && cmake --build build -j
./build/gd-tool-02                 # or: ./build/gd-tool-02 song.mp3 | stems-folder/ | --simple
```

## Models (fully offline)

Nothing is downloaded at run time: the separator reads the Demucs weights from
`models/` and blocks Hugging Face access (`HF_HUB_OFFLINE=1`). The files are the
`adefossez/HTDemucs-6s` Hugging Face repo (MIT license), copied once:

| File | SHA-256 |
|---|---|
| `models/htdemucs_6s.yaml` | `207405151270af8fd81c2373c25d27950916682ac91dca7884a11ce13dad6f58` |
| `models/5c90dfd2.safetensors` (~105 MB, git-ignored) | `d2a1745f0744721f6b8ca5bf469b67c651ea5ed1b52998cab033b2158609d411` |

The separation is seeded, so the same song always gives the same stems.

Keys: see **Help > Keyboard Shortcuts** (F1).

Debug flags (for screenshots in tests): `--settings FILE`, `--screenshot FILE.bmp`,
`--tab N`, `--menu N`, `--hover X Y`, `--wait-jobs`, `--seek S`, `--part NAME`, `--forge`.
