# gd-tool-02 backlog

The single backlog of gd-tool-02 (the Dream Charter Engine app). Read and
update it at the start and at the end of every task. IDs are stable.

Status: ✅ done · 🟡 in progress / partly done · ❌ not started · ⏸️ parked

## v1 rework (spec: [product_spec.md](product_spec.md), started 2026-11-19)

| ID | Item | Status | Notes |
|---|---|---|---|
| R01 | Guidelines, spec and backlog files in `docs/` | ✅ | |
| R02 | Load a song: spinning disc + progress bar while the metadata is read; the song becomes a project | ✅ | `drawLoading`, PROGRESS lines from extract_metadata.py |
| R03 | Step bar at the top: Song home > Song forge > Fine tuning > Export song (current highlighted, done ones checked) | ✅ | Tuning/Export unlock once forged; shrinks on narrow windows |
| R04 | Song home: guitar / bass / drums / lyrics boxes, several sheets each, choose which to use; FORGE SONG button | ✅ | drop on a box or + Add; checkboxes (lyrics: one choice); Details + lyrics editor overlays; keys toggle |
| R05 | Sheets: Guitar Pro and MIDI scores guide the engine (one file per instrument, merged) | 🟡 | R16 found .gp3-.gp5 needed `pyguitarpro` (now in requirements). A file in several boxes is aligned once. Still not run on a real tab of a song we have |
| R06 | Sheets: chord sheets (cifra) limit the chords and can give the lyrics | ✅ | verified in a forge run (test chord sheet) |
| R07 | Sheets: existing charts (.chart, notes.mid with PART tracks) used as-is for that part | ✅ | unit-tested parsers; assumes the chart was made for this audio (L02) |
| R08 | Song forge: separation (if needed) + charting with spinning disc + progress bar; stems mixer + chart preview after | ✅ | full run 90 s on M-series (Demucs + engine) |
| R09 | Fine tuning: note editor for guitar / bass / drums (add, delete, move, sustain, HOPO, star power, cymbal), undo | ✅ | `--selftest` drives it (23 checks); highway preview |
| R10 | Fine tuning: vocals (re-pitch, move, lyric text) | ✅ | phrases are extended on export for moved/added syllables |
| R11 | Export song: Clone Hero and YARG, targets GH1 / GH2-GH3-Aerosmith / Full band, OGG stems | ✅ | Clone Hero and YARG folders are the same layout; not yet loaded in the games themselves (L08) |
| R12 | Export to the Additive menu folder (gd-tool-01) | ⏸️ | Disabled in v1: gd-tool-01 has no format yet |
| R13 | Linux build + tests (podman, Fedora) | ✅ | `scripts/linux_test.sh` |
| R14 | Band view: guitar, bass, drums (+ keys) highways and the vocals at the same time | ✅ | Song forge default; Mixer toggles the side panels; One part = old single view |
| R15 | Evil Papagali: find online resources (tabs, chords, lyrics, charts) to make its song better | 🟡 | Research done: [research/evil_papagali.md](research/evil_papagali.md). Best: a human RB3 chart on RhythmVerse; needs a browser download (+ L12) |
| R16 | Paste button on the Guitar / Bass / Drums boxes: a copied file, a chord sheet or a text (ASCII) tab; text tabs become a MIDI score that guides the forge | ✅ | also Cmd/Ctrl+V over a box; text tabs only correct pitches/chords ("corrigir"), never rhythm; a tab that doesn't match the song is ignored |
| R17 | Paste opens a text editor: paste and edit the clipboard, then add it to the box (pasted text sheets can be edited again) | ✅ | Overlay::Paste with the shared TextEdit; live "text tab / chord sheet" detection; edit button on .txt sheets |
| R18 | The app speaks English only: the engine's (Portuguese) messages are translated or kept in forge/forge.log | ✅ | forge.translate / english_error; everything else in forge/forge.log; English genre names |
| R19 | A tab drives the guitar / bass: notes come only from the sheet (score-only), never from the audio; a tab that doesn't match charts nothing | ✅ | ScoreOnly wraps the engine's Guide (fidelity seguir, drops notes outside the score's span, nothing when it doesn't match); text tabs snapped to 16ths. User's Evil Papagali tab: 73% match, 1245 guitar notes from the tab |
| R20 | Guitar and bass need a sheet (tab, MIDI, chart or chord sheet) or a "This song has no guitar/bass" checkbox under the box; FORGE is blocked otherwise. Chords alone: audio limited to those chords | ✅ | forgeBlocker (C++) + Plan (Python) |
| R21 | MIDI files in every box (as a score, or as a chart with PART tracks) | ✅ | tested with a MIDI score of the Evil Papagali tab: same result as the text tab |
| R22 | Band view: a checkbox over each highway (and the vocals) turns that instrument's sound on / off | ✅ | toggles the stem the part is made from |
| R23 | Vocals always show (band view, Fine tuning, Export), even when empty | ✅ | |
| R24 | "This song has no drums" checkbox: drums are not forged | ✅ | `sheets: drums_absent` |
| R25 | No keys at all: remove "Chart keys too", the keys part (forge, band view, Fine tuning, Export, imported charts) | ✅ | the keys stem stays as audio (song.ogg); PART KEYS of imported charts is ignored |
| R26 | App volume slider in the bottom bar, independent of the OS volume (remembered) | ✅ | SDL_SetAudioDeviceGain on the app's device; settings.yaml volume |
| R27 | Chart offset per instrument (band view): shift that chart's notes, or the whole song's audio against the charts; the chart adapts (Fine tuning, Export) | ✅ | chart offset = notes shifted in chart.txt (+ forge.<part>_offset_ms, re-applied by forge.py, kept in undo); song offset = StemPlayer offset + export pads / trims (forge.audio_offset_ms) |
| R28 | "Rest of the mix" checkbox: hear / mute the stems no charted part uses | ✅ | |
| R29 | No lyrics -> no vocals track (band view, Fine tuning, Export) | ✅ | vocals strip / tab only when lyricsChoice != none (or the chart has vocals) |
| R30 | Screens layout board: every screen as movable blocks, for the owner to reorganize; then rebuild the app to match | 🟡 | Board: https://claude.ai/artifact/T6m7EsPGnQwWF5rnFeMc9C (18 frames, 327 blocks, built 2026-11-19). Waiting for the owner's layout |
| R31 | Guitar Pro 5 (.gp5) support, proven end to end with a real file (Song home, forge, tests on Mac and Linux) | ✅ | Real GP5 of the Evil Papagali tab (guitar + drums tracks): 73% match, 1245 guitar notes, drums driven by its drum track. Fixed: copies of one file in several boxes were aligned twice (now matched by content). Tests: python/tests/test_gp5.py |
| R32 | A Guitar Pro / MIDI score fills every box it has an instrument for (guitar, bass, drums), shown in each | ✅ | python/inspect_sheet.py (Inspect job) copies the file to every box it has an instrument for; clears that box's absent tick; works for charts too |
| R33 | Several guitar tracks: prefer the solo / lead one; when unsure, the user picks the track (FORGE waits) | ✅ | sheets.default_track (lead / solo / solista / guitar 1 by name); track picker on the row, saved as sheets.<box>_track; FORGE blocked until picked |
| R34 | Song forge: a "Difficulties" view, Easy..Expert side by side for one instrument | ✅ | forgeView_ 2 (drawDifficulties) |
| R35 | Remove the forge settings panel (Mixer and the not-forged screen); keep the defaults | ✅ | drawForgeSettings removed (Mixer and not-forged screen); defaults stay (steady drums / riffs, auto genre) |
| R36 | Keys and "other" are never separate stems: always inside the rest of the mix | ✅ | separate_stems merges piano into other; old projects' keys.wav mixed into other in the player |
| R37 | Guitar Pro 4 (.gp4) support, proven like GP5 | ✅ | every Guitar Pro test runs for .gp4 and .gp5; a real GP4 of Evil Papagali (2 guitars, bass, drums) filled all boxes, asked for the guitar, drove guitar / bass / drums |
| R38 | A partial Guitar Pro tab (Lets Ride To Metal Land: 40 bars = 1:16 of a 4:46 song) charted nothing for guitar / bass | ⏸️ | Tried fitting short sheets in the engine (written tempo, covered span, song start); the owner rejected the result: "back to how it was". Reverted. Short sheets are now reported instead (R39) |
| R39 | Popup when a Guitar Pro / MIDI sheet's music is much shorter than the song (< 70%): once per file | ✅ | sheets.music_seconds (beats at the written tempo; a chart's last note) in the .tracks cache; checkShortSheets / Overlay::Notice; remembered in sheets.short_warned |

## Later

| ID | Item | Status | Notes |
|---|---|---|---|
| L01 | Several reference tabs for the same instrument: pick per bar across files | ❌ | v1 uses the first checked file per instrument |
| L02 | Existing chart imported with another audio: find the offset | ❌ | |
| L03 | Fine tuning: regenerate Easy/Medium/Hard from an edited Expert | ❌ | engine reductions |
| L04 | YARG `.sng` package | ❌ | |
| L05 | A real TTF font (accents, nicer text) | ❌ | SDL debug font is ASCII only |
| L06 | Windows build (paths, Python venv location, dialogs) | ❌ | |
| L07 | Export a lead-in (silence before the first note) | ❌ | Clone Hero is fine without |
| L08 | Load an export in Clone Hero and YARG on a real install and play it | ❌ | needs the games installed; notes.mid round-trips to chart.txt exactly |
| L09 | Licence: MMS_FA (CC BY-NC 4.0) and ADTOF (CC BY-NC-SA) weights are non-commercial | ⏸️ | Decide with the project licence |
| L10 | Windows / Android runs | ❌ | not tested; Windows python path is coded (`.venv/Scripts/python.exe`) |
| L12 | Import Rock Band packages: CON (Xbox STFS) and `.rba`/`.mogg`, so RB3 customs from RhythmVerse can be dropped on Song home | ❌ | R15: the best Evil Papagali source is an RB3 custom |
| L14 | A partial tab leaves the rest of the song without guitar / bass (rule R19) | ⏸️ | The owner wants complete sheets, not fitting: see R39 |
| L13 | Drum-only references (a drum text tab, a drums-only MIDI) can't be aligned: the engine aligns scores by their pitched tracks | ❌ | Works when the drums come in a full score (Guitar Pro / MIDI) also used for guitar or bass. Needs onset-based alignment |
| L11 | Old projects (before the rework) have `forge/preview.txt` only: they show as not forged until forged again | ⏸️ | forge again, or convert with `forge.py --preview-only` |

## Bugs found in the rework (fixed)

| ID | Bug | Status | Notes |
|---|---|---|---|
| B01 | Forge downloaded the 1.26 GB MMS_FA model on its first lyrics run (must be offline) | ✅ | `forge.block_downloads()`; model kept in models/torch (git-ignored), test |
| B02 | libsndfile crashed (segfault) writing a whole song to OGG in one call | ✅ | write in blocks; test with 4 minutes of audio |
| B03 | `SDL_LoadPNG` needs SDL 3.4; Linux distros ship 3.2 (build broke) | ✅ | BMP for the window's images (disc, cover.bmp) |

## Done before the rework (spike)

| ID | Item | Status | Notes |
|---|---|---|---|
| S01 | Stems: Demucs htdemucs_6s offline, seeded | ✅ | |
| S02 | Metadata extraction to project.yaml | ✅ | |
| S03 | Forge via ai_charter, Beat This! + ADTOF | ✅ | |
| S04 | Steady drums (repeat_similarity) and steady riffs (consolidate_pitched) | ✅ | |
