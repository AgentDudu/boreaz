#pragma once

#include <cstddef>
#include <string_view>

#include "boreaz/engine/params.h"

namespace boreaz::engine {

// One row per engine parameter: the name a file uses, the name a human reads, what the number is
// measured in, the range it may not leave, the value it starts at, and where it lives.
//
// This is the whole reason phase 7 exists. Before it the ranges were in `Params::clamp_all`, the
// names were in the parameter file reader, the defaults were in the struct's initialisers, and a
// slider would have been a fourth copy. Four copies of a list is how a parameter ends up reachable
// from a config file and greyed out in the window, so the list is written once here and everybody
// else walks it: adding a parameter is one row, one field, and no other edit.
struct ParamSpec {
    const char* key; // the name in params.ini, in a patch, and behind a flag: --gain is this row
    const char* label; // the name in the panel, in words a player uses
    const char* unit;  // appended to the number: "", " dB", " Hz", " s", " st"
    float minimum;
    float maximum;
    float default_value;
    // Exactly one of these two is set, which is how a whole number and a real one share a table.
    float Params::* real;
    int Params::* integer;
};

// The table, in the order the panel draws it. Logarithmic parameters are drawn as themselves;
// a slider for a decay time is a lie either way and pretending otherwise costs a column.
const ParamSpec* param_table(std::size_t* count);

// The row for `key`, or nullptr: an unknown name in a parameter file is an error the user has to
// see, so the caller has to be able to ask.
const ParamSpec* param_spec(std::string_view key);

float param_read(const ParamSpec& spec, const Params& params);

// Always lands inside [minimum, maximum]: a slider, a text file and a MIDI controller are not
// allowed to disagree about what a legal value is, and none of them may make a filter non-finite.
void param_write(const ParamSpec& spec, float value, Params& params);

// Move `from` one block's worth toward `to` - alpha = 1 - exp(-block_seconds / tau), so the time
// constant does not change when the buffer does. Zipper smoothing, named after the potentiometer
// that clicks when you move it fast: a parameter that jumps from 0 to 1 on cutoff is a
// discontinuity in a resonant filter, and a discontinuity is a click.
//
// Integers are assigned, not ramped. A transpose of 3.5 semitones is not a thing anybody asked for;
// the smooth version of a transpose is the voice ramping the frequency it is already sounding, and
// that is the voice's business, not this function's.
void param_slew(Params& from, const Params& to, float alpha);

} // namespace boreaz::engine
