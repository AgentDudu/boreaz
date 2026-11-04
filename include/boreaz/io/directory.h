#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace boreaz::io {

// One thing a folder offered the player who is looking for a song: a folder to walk into, or a file
// this app can open.
struct FolderEntry {
    std::string name; // what to show
    std::string path; // what to ask for, already joined - the caller never builds a path
    bool folder = false;
};

// What one folder holds that a song picker can offer: folders first, then the files this app reads
// (.mid and .midi, said without regard to case because the names on this platform are not
// case-sensitive), each group sorted without regard to case.
//
// This is the app's job and not the window's for the usual two reasons: a window that reads a
// directory is a window that cannot be tested without a screen, and a listing is data - it wants an
// assertion, not an eye test.
//
// `limit` is not decoration. The thread that pumps the window is the thread that feeds the audio
// ring, and a directory walk over a folder of 30 000 files on that thread is a stutter you can
// hear. The first `limit` offers are returned and `more` says how many there were after them - or,
// when the walk reached its own bound first, at least how many: a bounded walk cannot count what it
// did not look at, and the picker says "at least" rather than inventing a number.
//
// A folder that is not there, or cannot be read, answers false with the reason in the caller's own
// words to show. Nothing throws: the walk stops at the first entry it cannot read and keeps what it
// already has, because a folder that changes under a walk is a folder being written, not an error
// worth losing a song over.
bool list_folder(const std::string& folder, std::vector<FolderEntry>& into, std::size_t limit,
                 std::size_t& more, std::string& error);

// The folder a path sits in, so the picker can open where the song already loaded lives. A path
// with no folder in it - a bare name typed at the field - answers with the current directory, which
// is where the app is standing anyway.
std::string parent_folder(const std::string& path);

// The folder the app is standing in, absolute, or "" when the question cannot be answered. The
// picker opens here rather than at "." because a dot has no parent: `parent_folder(".")` is ".", so
// a player pressing Left out of a seeded "." lands in the folder they were already looking at.
std::string current_folder();

// True for the names a song picker will offer as files. Public because the window has to say why a
// greyed-out name is greyed out, and two rules written twice are two rules that drift.
bool reads_as_song(const std::string& name);

// The drives this machine answers on, said the way Windows says them: "C:/", "D:/". The letters are
// probed rather than asked of a platform API, which keeps this file portable and costs 26 stat
// calls - a price worth paying when the alternative is a player climbing out of one drive a folder
// at a time. Empty on a system with no drive letters, which is what a non-Windows system is.
std::vector<std::string> drives();

// True when `needle` is inside `name` with the case ignored, and true for an empty needle. The
// picker narrows its own listing with this so the folding it sorts by is the folding it searches
// by: two rules about case would disagree with each other.
bool name_contains(const std::string& name, const std::string& needle);

// The same folder said absolutely, so the explorer's address bar shows something a player can read
// and paste back instead of "..". The input comes back unchanged when the filesystem cannot answer.
std::string absolute_folder(const std::string& path);

} // namespace boreaz::io
