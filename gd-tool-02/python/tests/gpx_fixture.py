"""Writes real Guitar Pro 6 (.gpx) files for the tests: a score.gpif (the GP6 XML) inside a BCFS
file system (4 KB sectors), compressed as BCFZ like Guitar Pro 6 saves it. Same notes as
gp5_fixture: (beat, MIDI pitch or GM drum note) on a 16th-note grid."""

from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape

GUITAR_LOW_TO_HIGH = [40, 45, 50, 55, 59, 64]  # GPIF strings: 0 = the lowest
BASS_LOW_TO_HIGH = [28, 33, 38, 43]
SECTOR = 0x1000
# General MIDI drum -> Guitar Pro 6 kit (element, variation): the inverse of the engine's
# GP6_DRUM_ELEMENTS (verified against alphaTab's full-song .gp5 / .gpx pair).
GP6_ELEMENT_OF = {36: (0, 0), 35: (0, 0), 38: (1, 0), 40: (1, 1), 37: (1, 2), 41: (5, 0), 45: (6, 0), 47: (7, 0),
                  48: (8, 0), 50: (9, 0), 42: (10, 0), 46: (10, 2), 44: (11, 0), 57: (12, 0), 49: (13, 0), 55: (14, 0),
                  51: (15, 0), 53: (15, 2), 52: (16, 0)}


def _place(pitch: int, tuning: list[int], used: set[int]) -> tuple[int, int] | None:
    best = None
    for string, open_pitch in enumerate(tuning):
        fret = pitch - open_pitch
        if 0 <= fret <= 24 and string not in used and (best is None or fret < best[1]):
            best = (string, fret)
    return best


def _gpif(tracks: list[tuple[str, list[int] | None, list[tuple[float, int]], bool, int]], bpm: int) -> bytes:
    beats_all = [b for _, _, notes, _, _ in tracks for b, _ in notes]
    bars = int(max(beats_all + [0.0]) // 4) + 1
    xml_tracks, master_bars, bar_xml, voice_xml, beat_xml, note_xml = [], [], [], [], [], []
    ids = {"bar": 0, "voice": 0, "beat": 0, "note": 0}
    bar_ids: list[list[int]] = [[] for _ in range(bars)]
    for ti, (name, tuning, notes, drums, program) in enumerate(tracks):
        props = f"<Properties><Property name=\"Tuning\"><Pitches>{' '.join(map(str, tuning))}</Pitches></Property></Properties>" if tuning else ""
        kit = "<InstrumentSet><Type>drumKit</Type></InstrumentSet>" if drums else ""
        xml_tracks.append(f"<Track id=\"{ti}\"><Name>{escape(name)}</Name>{props}{kit}<GeneralMidi><Program>{program}</Program>"
                          f"<PrimaryChannel>{9 if drums else ti}</PrimaryChannel></GeneralMidi></Track>")
        by_slot: dict[int, list[int]] = {}
        for beat, pitch in notes:
            by_slot.setdefault(int(round(beat * 4)), []).append(pitch)
        for bar in range(bars):
            beat_ids = []
            for slot in range(bar * 16, bar * 16 + 16):
                note_ids, used = [], set()
                for pitch in sorted(by_slot.get(slot, [])):
                    if drums:  # GP6 stores a kit element and a variation, not a MIDI number
                        element, variation = GP6_ELEMENT_OF.get(pitch, (1, 0))
                        body = (f"<Property name=\"Element\"><Element>{element}</Element></Property>"
                                f"<Property name=\"Variation\"><Variation>{variation}</Variation></Property>")
                    else:
                        placed = _place(pitch, tuning, used)
                        if placed is None:
                            continue
                        string, fret = placed
                        used.add(string)
                        body = (f"<Property name=\"String\"><String>{string}</String></Property>"
                                f"<Property name=\"Fret\"><Fret>{fret}</Fret></Property>")
                    note_xml.append(f"<Note id=\"{ids['note']}\"><Properties>{body}</Properties></Note>")
                    note_ids.append(ids["note"])
                    ids["note"] += 1
                notes_tag = f"<Notes>{' '.join(map(str, note_ids))}</Notes>" if note_ids else ""
                beat_xml.append(f"<Beat id=\"{ids['beat']}\"><Rhythm ref=\"0\"/>{notes_tag}</Beat>")
                beat_ids.append(ids["beat"])
                ids["beat"] += 1
            voice_xml.append(f"<Voice id=\"{ids['voice']}\"><Beats>{' '.join(map(str, beat_ids))}</Beats></Voice>")
            bar_xml.append(f"<Bar id=\"{ids['bar']}\"><Voices>{ids['voice']} -1 -1 -1</Voices></Bar>")
            bar_ids[bar].append(ids["bar"])
            ids["voice"] += 1
            ids["bar"] += 1
    for bar in range(bars):
        master_bars.append(f"<MasterBar><Time>4/4</Time><Bars>{' '.join(map(str, bar_ids[bar]))}</Bars></MasterBar>")
    xml = ("<?xml version=\"1.0\" encoding=\"utf-8\"?><GPIF><GPVersion>6.0</GPVersion><Score><Title>Test</Title></Score>"
           f"<MasterTrack><Automations><Automation><Type>Tempo</Type><Linear>false</Linear><Bar>0</Bar>"
           f"<Position>0</Position><Visible>true</Visible><Value>{bpm} 2</Value></Automation></Automations></MasterTrack>"
           f"<Tracks>{''.join(xml_tracks)}</Tracks><MasterBars>{''.join(master_bars)}</MasterBars>"
           f"<Bars>{''.join(bar_xml)}</Bars><Voices>{''.join(voice_xml)}</Voices><Beats>{''.join(beat_xml)}</Beats>"
           f"<Notes>{''.join(note_xml)}</Notes><Rhythms><Rhythm id=\"0\"><NoteValue>16th</NoteValue></Rhythm></Rhythms></GPIF>")
    return xml.encode("utf-8")


def _bcfs(files: dict[str, bytes]) -> bytes:
    """A BCFS file system: sector 0 a header, one sector per file entry (type 2, name, size,
    the list of its data sectors), then the data sectors."""
    sectors = [bytes(SECTOR)]
    entries, data_sectors = [], []
    next_sector = 1 + len(files)
    for name, data in files.items():
        chunks = [data[i:i + SECTOR] for i in range(0, len(data), SECTOR)] or [b""]
        blocks = list(range(next_sector, next_sector + len(chunks)))
        next_sector += len(chunks)
        entry = bytearray(SECTOR)
        entry[0:4] = (2).to_bytes(4, "little")
        entry[4:4 + len(name)] = name.encode("latin-1")
        entry[0x8C:0x90] = len(data).to_bytes(4, "little")
        for k, block in enumerate(blocks):
            entry[0x94 + 4 * k:0x98 + 4 * k] = block.to_bytes(4, "little")
        entries.append(bytes(entry))
        data_sectors += [c.ljust(SECTOR, b"\0") for c in chunks]
    return b"BCFS" + b"".join(sectors + entries + data_sectors)


class _BitWriter:
    def __init__(self) -> None:
        self.out, self.byte, self.n = bytearray(), 0, 0

    def bit(self, value: int) -> None:
        self.byte = (self.byte << 1) | (value & 1)
        self.n += 1
        if self.n == 8:
            self.out.append(self.byte)
            self.byte, self.n = 0, 0

    def bits(self, value: int, count: int) -> None:  # high bit first
        for i in range(count - 1, -1, -1):
            self.bit(value >> i)

    def bits_reversed(self, value: int, count: int) -> None:  # low bit first
        for i in range(count):
            self.bit(value >> i)

    def done(self) -> bytes:
        while self.n:
            self.bit(0)
        return bytes(self.out)


def _bcfz(data: bytes) -> bytes:
    """BCFZ: the expected size, then literal runs (flag 0, 2-bit count, bytes); valid for any
    reader, Guitar Pro's included, although nothing is back-referenced."""
    w = _BitWriter()
    for i in range(0, len(data), 3):
        chunk = data[i:i + 3]
        w.bit(0)
        w.bits_reversed(len(chunk), 2)
        for byte in chunk:
            w.bits(byte, 8)
    return b"BCFZ" + len(data).to_bytes(4, "little") + w.done()


def write_gpx(path: Path, guitar: list[tuple[float, int]], bpm: int = 140,
              bass: list[tuple[float, int]] | None = None, drums: list[tuple[float, int]] | None = None,
              guitar_name: str = "Distortion Guitar",
              more_guitars: list[tuple[str, list[tuple[float, int]]]] | None = None, compress: bool = True) -> Path:
    tracks = [(guitar_name, GUITAR_LOW_TO_HIGH, guitar, False, 30)]
    tracks += [(name, GUITAR_LOW_TO_HIGH, notes, False, 30) for name, notes in (more_guitars or [])]
    if bass:
        tracks.append(("Bass", BASS_LOW_TO_HIGH, bass, False, 33))
    if drums:
        tracks.append(("Drums", None, drums, True, 0))
    container = _bcfs({"score.gpif": _gpif(tracks, bpm)})
    path.write_bytes(_bcfz(container) if compress else container)
    return path
