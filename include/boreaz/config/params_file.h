#pragma once

#include <string>

#include "boreaz/engine/params.h"

namespace boreaz::config {

// Where parameters come from: `key = value`, one per line, `#` starts a comment. The same shape as
// the patch files the preset phase will ship, because a render parameter file and a preset are one
// thing with two extensions - and one table, which is why this is a library rather than a copy in
// whichever tool needs it next.
//
// An unknown key is an error rather than a shrug: a misspelled parameter that is quietly ignored is
// a preset that does not sound the way its author named it.
bool load_params_file(const std::string& path, boreaz::engine::Params& params, std::string& error);

// One parameter, set from text, by way of the same table, the same parser and the same messages as a
// parameter file. `flag` is what the error should call it ("--gain"), so a flag that refuses sounds
// like the flag that was typed and not like a line in a file nobody opened. An unknown key, a value
// that is not a number, and a number outside the parameter's own range are all refused.
bool set_param_from_text(const std::string& flag, const std::string& key, const std::string& value,
                         boreaz::engine::Params& params, std::string& error);

// The instrument's own defaults, `config/params.ini`, looked for in the working directory and then
// up to three levels above it. A build tree lives below the repository, and the two ways of running
// a build are from the repository root or from build/, so this is the difference between coming up
// with the shipped defaults and coming up with none. When nothing is found the relative path comes
// back unchanged, which is the name the load error will print.
std::string defaults_path();

// Every key the engine has, in the engine's own order, as one comma separated line. The message
// that refuses an unknown key promises the player a list, and this is the list - from the table, so
// it is never a turn behind the parameters that actually exist.
std::string known_keys();

} // namespace boreaz::config
