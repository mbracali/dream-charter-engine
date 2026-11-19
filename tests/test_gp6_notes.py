"""Guitar Pro 6 (.gpx) writes drum notes as a kit element + variation and vocal / keyboard notes
as a tone + octave; both become MIDI notes (verified against alphaTab's full-song .gp5 / .gpx)."""

from ai_charter.reference import GM_DRUMS, parse_gpif


def gpif(track: str, notes: list[str]) -> bytes:
    note_xml = "".join(f'<Note id="{i}"><Properties>{n}</Properties></Note>' for i, n in enumerate(notes))
    beats = "".join(f'<Beat id="{i}"><Rhythm ref="0"/><Notes>{i}</Notes></Beat>' for i in range(len(notes)))
    return (f'<GPIF><Tracks>{track}</Tracks><MasterBars><MasterBar><Time>4/4</Time><Bars>0</Bars></MasterBar></MasterBars>'
            f'<Bars><Bar id="0"><Voices>0 -1 -1 -1</Voices></Bar></Bars>'
            f'<Voices><Voice id="0"><Beats>{" ".join(map(str, range(len(notes))))}</Beats></Voice></Voices>'
            f'<Beats>{beats}</Beats><Notes>{note_xml}</Notes>'
            f'<Rhythms><Rhythm id="0"><NoteValue>Quarter</NoteValue></Rhythm></Rhythms></GPIF>').encode()


def drum(element: int, variation: int = 0) -> str:
    return (f'<Property name="Element"><Element>{element}</Element></Property>'
            f'<Property name="Variation"><Variation>{variation}</Variation></Property>')


def test_gp6_drum_elements_become_the_kit():
    xml = gpif('<Track id="0"><Name>Drums</Name><Instrument ref="drmkt"/></Track>',
               [drum(0), drum(1), drum(10), drum(10, 2)])
    track = parse_gpif(xml)[0][0]
    assert track.drums and track.guess == "drums"
    assert [GM_DRUMS[n.pitch] for n in track.notes] == ["kick", "snare", "hihat", "hihat"]
    assert [n.pitch for n in track.notes] == [36, 38, 42, 46]


def test_gp6_tones_and_octaves_become_pitches():
    xml = gpif('<Track id="0"><Name>Lead vocal</Name><Instrument ref="sprn-s"/></Track>',
               ['<Property name="Tone"><Step>10</Step></Property><Property name="Octave"><Number>5</Number></Property>',
                '<Property name="Tone"><Step>0</Step></Property><Property name="Octave"><Number>5</Number></Property>'])
    track = parse_gpif(xml)[0][0]
    assert track.guess == "vocals" and [n.pitch for n in track.notes] == [70, 60]
