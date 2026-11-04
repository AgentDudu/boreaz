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

// One parameter, set from text, by way of the same table, the same parser and the same messages as
// a parameter file. `flag` is what the error should call it ("--gain"), so a flag that refuses
// sounds like the flag that was typed and not like a line in a file nobody opened. An unknown key,
// a value that is not a number, and a number outside the parameter's own range are all refused.
bool set_param_from_text(const std::string& flag, const std::string& key, const std::string& value,
                         boreaz::engine::Params& params, std::string& error);

// The instrument's own defaults, `config/params.ini`, looked for in the working directory and then
// up to three levels above it. A build tree lives below the repository, and the two ways of running
// a build are from the repository root or from build/, so this is the difference between coming up
// with the shipped defaults and coming up with none. When nothing is found the relative path comes
// back unchanged, which is the name the load error will print.
std::string defaults_path();

// The directory patches are listed from and written to: `patches/` next to the repository or the
// release, found by the same walk up the tree that `defaults_path` does, because a build tree sits
// below the repository and the two ways of running a build are from the root or from `build/`. When
// there is no such directory yet it answers `patches` anyway, which is the name the caller creates
// on the first save and the name the failure message prints if it cannot.
std::string patches_dir();

// The directory the player's own patches are written to, and the only one the app ever writes a
// patch into. The factory patches in `patches_dir()` ship with the app, so a save that could land
// there is a save that can overwrite a voice somebody else named.
//
// `%APPDATA%\Boreaz\patches` on Windows, which is where PLAN puts everything the machine writes.
// With no APPDATA at all - a service session, or a profile with the variable cleared - the answer
// is `patches-user` under the working directory, which is the same shape the release zip has.
std::string user_patches_dir();

// Every key the engine has, in the engine's own order, as one comma separated line. The message
// that refuses an unknown key promises the player a list, and this is the list - from the table, so
// it is never a turn behind the parameters that actually exist.
std::string known_keys();

// Writing a patch, in the format the paragraph above already describes. `name` and `instrument` go
// in the header as comment lines, because a patch's name and the sampled instrument it wants are
// not engine parameters and the reader refuses unknown keys on purpose. A comment is the one place
// in this file the existing reader ignores, which keeps a patch loadable by `--params` and by
// `config/params.ini` with no new loading code at all.
//
// Every parameter is written, every time, in the table's own order, with the shortest text that
// reads back as the same number. That is what makes the round trip testable: save, load, save, and
// the two files are the same bytes. A file that wrote only the parameters that differ from a
// default would be smaller and would not survive a change to what the default is.
bool save_params_file(const std::string& path, const boreaz::engine::Params& params,
                      const std::string& name, const std::string& instrument, std::string& error);

// The two header fields, read back. A file with no header - every parameter file that has ever
// existed - reads as empty name and empty instrument and is otherwise a normal patch.
bool read_patch_meta(const std::string& path, std::string& name, std::string& instrument,
                     std::string& error);

} // namespace boreaz::config
