# gd-tool-02: Dream Charter Engine

Any MP3 / WAV becomes a playable Clone Hero / YARG (Guitar Hero / Rock Band style)
custom song. The spec is in [docs/product_spec.md](docs/product_spec.md), the backlog in
[docs/backlog.md](docs/backlog.md) and the working rules in
[docs/general_guidelines.md](docs/general_guidelines.md).

## The flow

Drop a song on the window (or **Settings > Open Song**). A record spins and a progress bar
fills while the song is read (`python/extract_metadata.py`: tags, cover, embedded lyrics,
duration, BPM, key, loudness). The song becomes a **project**, and its four screens show as
a step bar at the top (`Cmd/Ctrl+1..4`):

1. **Song home**: the song (cover, details, `Edit details`) and one box per music sheet:
   - **Guitar / Bass / Drums**: drop, `+ Add` or **Paste** any number of sheets and tick the
     ones the forge should use. **Paste** (or `Cmd/Ctrl+V` over a box) adds a file copied in
     Finder / a file manager, or opens a text editor holding the clipboard, to paste and fix a
     **text tab** (`e|--0--3--|` lines, drum lines like `HH|x-x-x-|`, `x4` repeats) or a **chord
     sheet** before adding it; pasted text sheets have an **edit** button.
   - **Guitar and bass need a sheet** (or the box's **This song has no guitar / bass**
     checkbox); FORGE stays off until they have one:
     - a Guitar Pro tab (`.gp3 .gp4 .gp5 .gpx .gp .gp6-8`), a MIDI score or a text tab gives the
       **notes**: the part is the sheet's notes only, lined up with the recording; a sheet that
       doesn't match the recording charts nothing (never a guess from the audio);
     - an existing chart (`.chart`, or a `.mid` with `PART` tracks) is used **as-is** (it must
       be made for this audio: no offset search yet);
     - a chord sheet alone (`.txt`, Cifra Club / Ultimate Guitar style): the forge listens to
       the stem and uses only those chords; its lyrics are sung when nothing else gives lyrics;
     - a file in several boxes (a full Guitar Pro score in Guitar and Drums) is aligned once.
   - **Drums** come from the drums stem (ADTOF); a drum sheet is optional, and a drum text tab
     alone can't be lined up yet.
   - **Lyrics**: typed / pasted text (`Type / paste`), or `.txt` / `.lrc` files (LRC
     timestamps are dropped); pick one, or No vocals.
   - **Chart keys too** adds a keys part (Full band export only). **FORGE SONG** goes on.
2. **Song forge** (`python/forge.py`): separates the stems if needed (Demucs, offline),
   then charts every part with dream-charter-engine (`ai_charter`) and the sheets. The
   record spins with a progress bar. Then the **Band** view: the vocals across the top and
   every instrument's highway side by side (guitar, bass, drums, keys), all on one playhead,
   Easy..Expert; a checkbox over each part turns its sound on / off, and the vocals always show. **Mixer** shows the stems mixer (mute / solo / waveforms) and the forge
   settings beside it; **One part** shows a single big highway instead. The settings:
   - **Rules**: the engine's genre presets (`ai_charter/genres/`);
   - **Drums**: Heard 0.9 / **Steady** 0.8 / Locked 0.7 (engine `repeat_similarity`: bars at
     least this alike get the same notes);
   - **Riffs**: **Steady** groups repeating guitar / bass / keys riffs (`consolidate_pitched`,
     0.8) / Heard leaves each bar as detected;
   - **Guitar / Bass**: the stem each is charted from; **Listen**: full mix or this part.
   **Forge again** lights up when settings or sheets changed.
3. **Fine tuning**: a piano-roll editor of `forge/chart.txt`, with a highway preview.
   - Guitar / bass / drums: click an empty spot to add a note (snapped to the grid: 1/4 ..
     1/32, triplets), drag to move (time and lane), drag a tail's end (or Alt+drag) for
     sustain, right click or Delete to remove, **H** HOPO, **S** Star Power, **C** cymbal;
     arrows nudge the selected note.
   - Vocals: drag to move / re-pitch, drag the right edge to resize, click to add; Enter
     edits the syllable's text (Tab: next one; `-` joins a word, `+` is a slide).
   - Undo / redo (`Cmd/Ctrl+Z`, `Shift+Z`), **Revert to forged**; every edit is saved.
4. **Export song** (`python/export_song.py`): one Clone Hero / YARG folder per target, by
   the original setlists:
   - **Guitar Hero 1**: guitar (`guitar.ogg` + `song.ogg`);
   - **GH2 / GH3 / Aerosmith**: guitar + bass (`guitar`, `bass`, `song`);
   - **Full band**: every charted part, one stem each (guitar, bass, drums, vocals, keys).
   Each folder has `notes.mid`, `song.ini` (difficulties, length, preview), `album.png` and
   Vorbis `.ogg` stems (no ffmpeg). It goes to the Clone Hero / YARG songs folder set in
   Settings, or to `PROJECT/export/<format>/`. **Send to the Additive menu** (gd-tool-01)
   is disabled until that tool defines its format.

## A project folder

```
Song name/
  project.yaml   metadata + settings (song, analysis, audio, stems, sheets, forge, export)
  song.mp3       the copied song           cover.png (+ cover.bmp for the window)
  lyrics.txt     typed lyrics              sheets/<guitar|bass|drums|lyrics>/  the sheets
  stems/         mix, guitar, bass, drums, keys, vocals, other (.wav)
  forge/         <Artist - Title>/notes.mid (the engine's; its tempo map is reused),
                 chart.txt (what Fine tuning edits), forged.txt (the forge's own copy)
  export/        exports, when no songs folder is set
```

`chart.txt` is plain text, one event per line, in seconds of the song
(`python/chart_text.py` documents it); `python/midi_io.py` converts notes.mid <-> chart.

## Code

C++17 + SDL3 (like gd-engine-launcher), drawn with SDL_Renderer and SDL's built-in pixel
font (ASCII only: "Canção" shows as "Cancao"; files keep the real text).

| File | What |
|---|---|
| `src/app.h`, `src/app.cpp` | the App: settings, projects, jobs, input, shell (menu, sidebar, step bar), widgets |
| `src/screen_home.cpp` | Song home: sheets, details, lyrics editor |
| `src/screen_forge.cpp` | Song forge: progress, mixer, settings, preview |
| `src/screen_tune.cpp` | Fine tuning: the note / vocal editor |
| `src/screen_export.cpp` | Export song |
| `src/board.cpp` | the highway, vocal board and overview |
| `src/chart.*` | chart.txt in C++ (load, save, beat grid) and drawing shapes |
| `src/stems.*`, `src/worker.*`, `src/yaml_doc.*`, `src/fields.h`, `src/text_util.h` | playback, Python jobs, YAML subset, project.yaml fields, UTF-8 |
| `python/` | `extract_metadata.py`, `separate_stems.py`, `forge.py`, `sheets.py`, `ascii_tab.py`, `chart_text.py`, `midi_io.py`, `export_song.py` |

The window is English only: the engine (`ai_charter`) logs in Portuguese, so the forge
writes its log to `forge/forge.log` and shows English lines only (`forge.translate`).

The Python helpers print one event per line (`STATUS`, `PROGRESS 0..1`, `STEM`, `COUNT`,
`EXPORTED`, `DONE`, `ERROR`), read by `src/worker.*`. The app logs its jobs to
`gd-tool-02.log` next to `settings.yaml` (`~/Library/Application Support/mbracali/gd-tool-02/`
on macOS).

## Setup (macOS)

```bash
brew install sdl3 cmake
python3.12 -m venv .venv && .venv/bin/pip install -r python/requirements.txt
cmake -S . -B build && cmake --build build -j
./build/gd-tool-02                 # or: ./build/gd-tool-02 song.mp3
```

## Tests

```bash
ctest --test-dir build                      # C++: Python job output, chart.txt + beat grid
.venv/bin/python -m pytest -q python/tests  # Python: chart <-> MIDI, sheets, .chart, export, offline
scripts/linux_test.sh                       # Linux (Fedora container, podman or docker): build,
                                            # both test suites, a headless run of every screen
```

Debug flags (screenshots): `--settings FILE`, `--screenshot FILE.bmp`, `--screen 0-3`,
`--part NAME`, `--seek S`, `--menu N`, `--hover X Y`, `--overlay details|lyrics|help`,
`--forge`, `--wait-jobs`, `--simple`, `--one-part`, `--panels`, `--selftest`, `--paste BOX`.

## Models (fully offline)

Nothing is downloaded at run time: the separator reads Demucs from `models/`
(`HF_HUB_OFFLINE=1`), and the forge refuses any download (`forge.block_downloads`): a
missing model is skipped and the engine uses its fallback.

| File (git-ignored unless noted) | What | License | SHA-256 |
|---|---|---|---|
| `models/htdemucs_6s.yaml` (in git) | Demucs 6-stem bag | MIT | `207405151270af8f...` |
| `models/5c90dfd2.safetensors` (~105 MB) | Demucs weights (`adefossez/HTDemucs-6s`) | MIT | `d2a1745f0744721f...` |
| `models/torch/hub/checkpoints/beat_this-final0.ckpt` | Beat This! (beats, bar starts) | MIT | `8c328b45f59d8dd3...` |
| `models/torch/hub/checkpoints/model.pt` (~1.26 GB) | torchaudio MMS_FA (lyrics alignment) | CC BY-NC 4.0 | `20ef12963ab4924b...` |
| inside the `adtof-pytorch` package | ADTOF drum transcription | CC BY-NC-SA | |

Without MMS_FA the engine places the lyrics with its older aligner; without Beat This! it
uses librosa's beat tracker (no bar starts); without ADTOF, rules-only drum lanes. MMS_FA
and ADTOF are non-commercial licenses (see the backlog's licence item).
