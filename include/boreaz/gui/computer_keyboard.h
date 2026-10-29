#pragma once

namespace boreaz::gui {

// The computer keyboard as a two-octave piano.
//
// GLFW's key codes for letters and digits *are* the ASCII codes, which is what lets this table live
// here with no GLFW in front of it: the mapping is arithmetic on the same integers the callback
// reports, so it is testable without a window, a display, or a pointer to move.
//
// The rows follow the layout nearly every software piano uses, because it is the one a player who
// has never seen this program has already seen somewhere: the bottom letter row is the octave below
// middle C, the row above it is middle C upwards, with the home-row keys as the sharps in between.
//
//   z s x d c v g b h n j m        C  C# D  D# E  F  F# G  G# A  A# B     (48-59)
//   q 2 w e 3 r 5 t 6 y 7 u        C  C# D  D# E  F  F# G  G# A  A# B     (60-71)
//   i 9 o 0 p                      C  C# D  D# E                           (72-76)
//
// Notes outside the two octaves are played by the mouse, a real MIDI keyboard, or the transpose
// control; the point of this table is that a desk with no instrument still makes a sound.
inline int note_for_computer_key(int key_code) {
    switch (key_code) {
    case 'z':
        return 48;
    case 's':
        return 49;
    case 'x':
        return 50;
    case 'd':
        return 51;
    case 'c':
        return 52;
    case 'v':
        return 53;
    case 'g':
        return 54;
    case 'b':
        return 55;
    case 'h':
        return 56;
    case 'n':
        return 57;
    case 'j':
        return 58;
    case 'm':
        return 59;
    case ',':
        return 60;
    case 'l':
        return 61;
    case '.':
        return 62;
    case 'q':
        return 60;
    case '2':
        return 61;
    case 'w':
        return 62;
    case 'e':
        return 63;
    case '3':
        return 64;
    case 'r':
        return 65;
    case '5':
        return 66;
    case 't':
        return 67;
    case '6':
        return 68;
    case 'y':
        return 69;
    case '7':
        return 70;
    case 'u':
        return 71;
    case 'i':
        return 72;
    case '9':
        return 73;
    case 'o':
        return 74;
    case '0':
        return 75;
    case 'p':
        return 76;
    default:
        return -1; // not a key this program has promised anything about
    }
}

// Space is the sustain pedal: held down is a pedal, not a toggle, because that is the only thing the
// foot knows. The engine treats CC 64 above 63 as "down" and will hand the held notes to the next
// release, exactly as it does for a pedal on a hardware keyboard.
inline bool key_is_sustain_pedal(int key_code) {
    return key_code == ' ';
}

} // namespace boreaz::gui
