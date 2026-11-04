#pragma once

#include <string>

namespace boreaz::config {

// What the machine was set up with, as opposed to what the instrument sounds like: the audio port,
// the MIDI port, the sample library, and the patch that was open when the app last closed.
//
// These are not engine parameters, so this file has its own five keys and its own reader. Feeding
// it to the parameter parser would be wrong in the shape of the data, not in its details: that one
// refuses an unknown key because a half-read sound is a sound nobody wrote, and a setup file is not
// a sound. Nothing here reaches `Params`.
struct AppSettings {
    std::string audio_device; // what --device would take; empty is the system default
    std::string midi_port;    // what --midi would take, including "none" and "auto"
    std::string instrument;   // a sample library mapping; empty is the synthesiser
    std::string patch;        // the patch file that was last open, path and all
    // How fast the spectrum panel's hold line falls, in dB per second. A string like everything
    // else here, because this file's job is to remember what the machine said; the code that reads
    // the number is the code that has a range to check. Empty is the analyser's own default.
    std::string peak_decay;
};

// Where that file lives: `%APPDATA%\Boreaz\settings.txt`, next to the patches the player saves,
// which is where PLAN puts everything the machine writes. With no `APPDATA` at all it is
// `settings.txt` in the working directory - the same fallback `user_patches_dir()` settles for.
//
// It is not `config/params.ini` and it is not a patch. Those describe a sound; this one describes a
// machine, and a repository file cannot be a machine's setup without putting a path that exists on
// one computer into git.
std::string settings_path();

// Read it. A file that is not there is a first run and says nothing. A line this version does not
// know is skipped and named in `warning`, and the rest of the file is still used: a setup file must
// never be the reason the instrument cannot start. The value is everything after the first `=`, so
// a port name or a path with spaces in it needs no quoting.
void load_settings(const std::string& path, AppSettings& into, std::string& warning);

// Write it, every key every time - the way the patch writer does it. A file with keys missing is a
// file where a missing key means "whatever the default is this version", which is a different
// answer every release. Returns false with a reason: a setup file that cannot be written is worth
// saying, and is not worth stopping the app for.
bool save_settings(const std::string& path, const AppSettings& settings, std::string& error);

} // namespace boreaz::config
