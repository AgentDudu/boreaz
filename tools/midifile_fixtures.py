#!/usr/bin/env python3
"""Write the MIDI file fixtures that tests/test_midifile.cpp and boreaz-render are tested against.

    python tools/midifile_fixtures.py --verify boreaz midifile fixtures
    python tools/midifile_fixtures.py boreaz midifile fixtures
    python tools/midifile_fixtures.py --out some/other/dir boreaz midifile fixtures

The recipe header is the first line of tests/fixtures/midifile/README.md and is a required
argument: regenerating a generated asset should not be possible without reading the directory it
lands in. --verify compares what is committed against what this file says instead of writing, and
is the check CI runs.

The bytes are fully determined here: no clock, no random, no dictionary ordering. Two runs produce
identical files, which is what lets tests/golden/midifile.txt be five measurements instead of a
shrug, and what makes a broken fixture one command away from correct again.

Builders whose name begins with "raw_" produce a file that is deliberately wrong. They are still
byte exact and still deterministic, because a parser test that depends on where a truncation
happened to fall is a test that fails for reasons nobody can read.
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_DIR = REPO / "tests" / "fixtures" / "midifile"
RECIPE = "boreaz midifile fixtures"

TPQ = 480  # ticks per quarter note: the division of every fixture written here
A0, C4, C8 = 21, 60, 108  # the two ends of an 88 key piano, and middle C


def vlq(value):
    """Variable length quantity, MIDI's one variable width integer."""
    if value < 0x80:
        return bytes([value])
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def midi_file(fmt, tracks, division=TPQ):
    """One MThd and the track chunks, in order. The header counts the chunks it is given."""
    return (b"MThd" + (6).to_bytes(4, "big") + fmt.to_bytes(2, "big") +
            len(tracks).to_bytes(2, "big") + division.to_bytes(2, "big") + b"".join(tracks))


def deltas(events):
    """(absolute tick, message) to (delta tick, message), keeping the order inside one tick.

    That order is not arbitrary and the tests say so: a chord whose notes arrive in a stable order
    renders the same waveform twice, and sorting with a key that reorders ties would make a golden
    file a coin toss."""
    out = []
    previous = 0
    for tick, message in sorted(events, key=lambda event: event[0]):
        out.append((tick - previous, message))
        previous = tick
    return out


def encode_track(events, running_status=False):
    """A delta list into an MTrk chunk. With running_status on, a repeated status byte is dropped,
    which is the other half of what a real sequencer writes."""
    out = bytearray()
    last_status = None
    for delta, message in events:
        out += vlq(delta)
        status = message[0]
        if running_status and status & 0x80 and status == last_status:
            out += message[1:]  # the rule is about the status byte, not the whole message
        else:
            out += message
        last_status = status if status & 0x80 else None
    bytes_ = bytes(out)
    return b"MTrk" + len(bytes_).to_bytes(4, "big") + bytes_


# -- messages ---------------------------------------------------------------------------
#
# Each helper returns the bytes of one event, status byte included, so encode_track can decide
# whether to repeat it. Nothing here knows about ticks: a message is not an event until it has a
# delta in front of it.

def note_on(channel, note, velocity):
    return bytes([0x90 | channel, note, velocity])


def note_off(channel, note, velocity=64):
    return bytes([0x80 | channel, note, velocity])


def control(channel, number, value):
    return bytes([0xB0 | channel, number, value])


def bend(channel, value):
    """Pitch bend, -8192 to 8191, sent LSB then MSB because MIDI counts up."""
    raw = value + 8192
    return bytes([0xE0 | channel, raw & 0x7F, (raw >> 7) & 0x7F])


def program(channel, patch):
    return bytes([0xC0 | channel, patch])


def meta(kind, payload):
    return bytes([0xFF, kind]) + vlq(len(payload)) + payload


def tempo(microseconds):
    return meta(0x51, microseconds.to_bytes(3, "big"))


def time_signature(numerator, denominator_exponent):
    """The denominator arrives as a power of two, because that is how the specification sends it:
    4/4 is (4, 2), since 2 squared is 4."""
    return meta(0x58, bytes([numerator, denominator_exponent, 24, 8]))


def text(message):
    return meta(0x01, message.encode())


def sequence_name(name):
    return meta(0x03, name.encode())


def sysex(payload):
    """A system exclusive message: 0xF0, a length, a payload we drop, and a 0xF7 terminator.

    The terminator goes *outside* the length, which is one of the two spellings sequencers use - the
    other counts it. Both appear in files people actually exchange, so one of our fixtures is written
    this way and the parser is expected to have met the difference before the user did."""
    return bytes([0xF0]) + vlq(len(payload)) + payload + b"\xf7"


def end_of_track():
    return bytes([0xFF, 0x2F, 0x00])


# -- the files --------------------------------------------------------------------------
#
# Tests read these through boreaz::midifile and assert the resulting Song event by event, which
# means the shape of each file has one honest source. Change the recipe, run --verify, and read
# what the tests then say.

def lead_simple():
    """Eight ascending notes an eighth apart, one tempo, one track, and one thing of each kind we
    accept that is not a note: a volume wheel, and a pitch bend that leaves and comes back.

    This is the fixture that fails first when a key maps to the wrong pitch or a wheel lands on the
    wrong voice, so it says nothing else: no dropped parts, no format surprises, no tempo change.
    The last note is C8 minus an octave, which is the top of the melody a pianist would actually
    play twice."""
    melody = [(60, 40), (62, 50), (64, 60), (65, 70), (67, 80), (69, 90), (71, 100), (96, 110)]
    step = TPQ // 2
    events = [(0, tempo(500000)), (0, sequence_name("lead-simple")), (0, control(0, 7, 100))]
    for index, (note, velocity) in enumerate(melody):
        at = index * step
        events.append((at, note_on(0, note, velocity)))
        if note == 67:
            events.append((at, bend(0, 2000)))  # a third of a whole tone up, mid note
        if note == 69:
            events.append((at, bend(0, 0)))  # ...and back to where the wheel centres
        events.append((at + 200, note_off(0, note)))  # a note shorter than its slot, like a human
    events.append(((len(melody) - 1) * step + 200, end_of_track()))
    return midi_file(0, [encode_track(deltas(events))])


def pad_chords():
    """Four tracks holding a twelve note chord, a tempo change in a fifth track nobody plays, and
    four parts we have to drop: four program changes, a couple of text events, and one sysex.

    Format 1 with the conductor in its own track is what every sequencer exports, and it is the
    layout where a tempo map applied per track, or a note counted twice, shows up. All twenty four
    notes want to start at one sample, which is also the fixture that proves the voice pool steals
    in a file order a second render repeats."""
    voicing = [
        (0, [C4, C4 + 4, C4 + 7, C4 + 12, C4 + 16, C4 + 19]),
        (1, [C4 - 12, C4 - 8, C4 - 5, C4 + 24, C4 + 28, C4 + 31]),
        (2, [C4 + 2, C4 + 5, C4 + 9, C4 + 14, C4 + 17, C4 + 21]),
        (3, [C4 - 10, C4 - 7, C4 - 3, C4 + 26, C4 + 29, C4 + 33]),
    ]
    conductor = [
        (0, tempo(500000)),
        (0, time_signature(4, 2)),
        (0, text("pad-chords")),
        (8 * TPQ, tempo(750000)),  # slower, halfway through the first chord's hold
        (12 * TPQ, time_signature(3, 2)),
        (16 * TPQ, sysex(b"\x7e\x7f\x09\x01")),
        (16 * TPQ, end_of_track()),  # the conductor stops; the notes decide how long the file is
    ]
    tracks = [encode_track(deltas(conductor))]
    for channel, notes in voicing:
        events = [(0, program(channel, 89))]  # an organ patch: ignored, and said so
        for note in notes:
            events.append((2 * TPQ, note_on(channel, note, 70)))
            events.append((10 * TPQ, note_off(channel, note)))
        events.append((10 * TPQ, end_of_track()))
        tracks.append(encode_track(deltas(events)))
    return midi_file(1, tracks)


def bass_running_status():
    """Low staccato eighths with the status bytes dropped and the note offs spelled as note on with
    velocity zero, because that is what real bass files look like and both rules are easy to forget.

    Running status starts at the second event and never restarts, so the whole track after the
    first note relies on a status byte the parser has to remember for eight hundred bytes."""
    step = TPQ // 2
    events = [(0, tempo(500000))]
    pattern = [A0, A0 + 12, A0 + 19, A0 + 24]
    for step_number in range(16):
        at = step_number * step
        note = pattern[step_number % len(pattern)]
        events.append((at, note_on(0, note, 96)))
        events.append((at + step // 2, note_on(0, note, 0)))  # note off, MIDI's other spelling
    events.append((15 * step + step // 2, end_of_track()))  # no trailing silence: it ends where it ends
    return midi_file(0, [encode_track(deltas(events), running_status=True)])


def glissando_88():
    """Every key a piano has, A0 to C8, one per eighth note, with the tempo halved halfway up.

    This is the fixture that catches a key mapped to the wrong pitch, a note lost at the top or
    bottom of the range, and a tempo map applied to the wrong half of the file. The velocities climb
    from 40 to 120 so an ordered render can be checked key by key, and because the pairs are written
    in file order, a tie inside one tick is fixed: the previous key's note off arrives before the
    next key's note on, which is what makes the file's waveform repeatable."""
    step = TPQ // 4
    keys = range(A0, C8 + 1)  # 88 of them, both ends inclusive
    assert len(list(keys)) == 88
    conductor = [
        (0, tempo(500000)),
        (0, time_signature(4, 2)),
        (44 * step, tempo(1000000)),  # half tempo, halfway up the keyboard
        (44 * step, end_of_track()),
    ]
    notes = []
    for index, key in enumerate(keys):
        at = index * step
        notes.append((at, note_on(0, key, 40 + (80 * index) // 87)))
        notes.append((at + 2 * step, note_off(0, key)))
    notes.append(((87 + 2) * step, end_of_track()))
    return midi_file(1, [encode_track(deltas(conductor)), encode_track(deltas(notes))])


BUILDERS = [
    ("lead-simple.mid", lead_simple),
    ("pad-chords.mid", pad_chords),
    ("bass-running-status.mid", bass_running_status),
    ("glissando-88.mid", glissando_88),
]


def main(argv):
    """--verify compares what is on disk instead of writing; --out writes somewhere else.

    The recipe header is the last argument and must match exactly. A generated asset should not be
    refreshable by whoever typed the shortest command, because a fixture refreshed by accident stops
    being evidence and becomes a rumour about whatever the code did last."""
    args = list(argv)
    verify = False
    if args and args[0] == "--verify":
        verify = True
        args = args[1:]
    out = DEFAULT_DIR
    if args and args[0] == "--out":
        if len(args) < 3:
            print("--out wants a directory and then the recipe header", file=sys.stderr)
            return 2
        out = Path(args[1])
        args = args[2:]
    if len(args) != 1 or args[0] != RECIPE:
        print(f"usage: midifile_fixtures.py [--verify] [--out DIR] {RECIPE}", file=sys.stderr)
        print("the header is the first line of the recipe in that directory", file=sys.stderr)
        return 2

    drifted = []
    for name, build in BUILDERS:
        data = build()
        where = out / name
        if verify:
            if not where.exists() or where.read_bytes() != data:
                drifted.append(name)
            continue
        out.mkdir(parents=True, exist_ok=True)
        where.write_bytes(data)
        print(f"  {name:28s} {len(data):6d} bytes")

    if verify:
        if drifted:
            print(
                "fixture drift: " + ", ".join(drifted) +
                " do not match tools/midifile_fixtures.py. Run it, read the tests, commit both.",
                file=sys.stderr)
            return 1
        print(f"{len(BUILDERS)} fixtures match the recipe")
        return 0
    print(f"wrote {len(BUILDERS)} fixtures to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
