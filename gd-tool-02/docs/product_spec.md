# Dream Charter Engine: product spec (v1 rework, 2026-11-19)

The owner's spec, kept here so it survives context resets. Decisions made in
the follow-up questions are marked **[Q]**.

## What it is

Software that takes any MP3 / WAV and produces a fully playable Rock Band /
Guitar Hero custom song.

## Flow

1. The user opens the app and loads an MP3 (drop or Open).
2. A **spinning disc** and a **progress bar** show while the metadata is read.
3. The song becomes a **project** (one per song).
4. The project's screens, shown as a step bar at the top (the current one
   highlighted as the user goes):
   1. **Song home**: one box per music sheet: guitar, bass, drums, and lyrics.
      A **FORGE SONG** button at the bottom goes to Song forge.
      **[Q]** The instrument boxes take Guitar Pro tabs (.gp3/.gp4/.gp5/.gpx/.gp),
      MIDI (as a score, or an existing chart with PART tracks), chord sheets
      (Cifra Club / Ultimate Guitar .txt) and existing charts (.chart,
      notes.mid). The user can attach several and choose which to use (or
      all). The lyrics box takes typed or pasted text, or .txt / .lrc files.
   2. **Song forge**: mixes the sheets, stem separation (Demucs) and the other
      libraries into the best chart we can make. A spinning disc and a
      progress bar while it works. The result: the 4 stems and the charts,
      ready to see and preview. Then: Fine tuning.
   3. **Fine tuning**: add, remove and edit notes. Guitar, bass and drums are
      straightforward; for lyrics, re-pitch the notes and edit the lyrics.
      Then: Export song.
   4. **Export song**: export to Clone Hero and YARG format.
      **[Q]** "Export to the Additive menu folder" (gd-tool-01, still a
      concept) is shown disabled in v1.
      **[Q]** Smart targets, by the original setlists:
      - **GH1**: guitar only (guitar.ogg + song.ogg).
      - **GH2 / GH3 / Aerosmith**: guitar + bass (guitar, bass, song).
      - **Full band** (GH4+ / RB / YARG): guitar, bass, drums, vocals (+ keys),
        one stem each.

## Base

**[Q]** Build on the current gd-tool-02 app (C++17 + SDL3, Python helpers,
the ai_charter engine) and `res/record-disk-clipart-design-illustration-free-png.webp`
(the spinning disc).
