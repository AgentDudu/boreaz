// The transport's own clock: it starts on music, not on the driver.

#include "boreaz/gui/transport_clock.h"

namespace boreaz::gui {

void TransportClock::update(double position_seconds, bool input_present) {
    if (!running_) {
        if (!input_present)
            return; // silence has no position, so there is nothing for a transport to be doing
        running_ = true;
        started_at_ = position_seconds; // the first note is at zero, not at the device's uptime
    }
    if (halted_)
        return; // parked where the song ended
    if (position_seconds < started_at_)
        // The audio position went backwards, which happens when the device is reopened: a new
        // stream, a new zero. Without this the transport would read negative and a panel would spend
        // the rest of the session drawing a playhead off the left of its own window.
        started_at_ = position_seconds;
    seconds_ = position_seconds - started_at_;
}

void TransportClock::halt() { halted_ = true; }

void TransportClock::reset(double position_seconds) {
    started_at_ = position_seconds;
    seconds_ = 0.0;
    halted_ = false;
    // `running_` is left as it was: a clock that was hidden because nothing had been played yet
    // stays hidden, and setting it here would print 0:00.000 in an empty window, which is the
    // promise this class exists to avoid.
}

} // namespace boreaz::gui
