#pragma once

namespace boreaz::gui {

// When the transport is running, and what time it reads.
//
// The audio clock starts when the device opens, which is not when music starts: a window that has
// been sitting open for ten minutes has a position of six hundred seconds and nothing has been
// played. So the transport keeps its own zero, taken at the first thing that deserves one - a song
// loaded, or the first note from a keyboard - and it stops when told to, which is how a song that
// has finished parks at its end instead of scrolling into a future nothing occupies.
class TransportClock {
  public:
    // `position_seconds` is where the audio thread says it is. `input_present` means something is
    // asking for music: a song is loaded, or a note is sounding right now. It opens the transport,
    // and it opens it again after a park, because music coming back is a new song and nobody wants
    // to play over the length of the last one. It never closes it - a rolled chord has gaps in it,
    // and a transport that stopped between two notes of a slur would be worse than one that runs.
    void update(double position_seconds, bool input_present);

    // Park: stop advancing, keep reading. The end of a song does this, and the number left on
    // screen is then the song's length, which is a fact worth keeping on the screen.
    void halt();

    // Back to zero. `position_seconds` is the audio position this frame, because zero is a
    // difference against it: taking it from anywhere else makes the next frame read either a
    // negative number or the device's uptime. Clearing a park is deliberate - asking for zero after
    // a song ended means "from here", not "keep showing a frozen zero".
    void reset(double position_seconds);

    bool running() const { return running_; }

    // Zero before the transport runs, and seconds since it started afterwards. Panels draw from
    // this and never from the audio position, which is the difference between a transport and a
    // stopwatch.
    double seconds() const { return seconds_; }

  private:
    bool running_ = false;
    bool halted_ = false;
    double started_at_ = 0.0;
    double seconds_ = 0.0;
};

} // namespace boreaz::gui
