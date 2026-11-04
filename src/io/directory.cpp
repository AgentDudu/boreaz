// Listing a folder for the player who is looking for a song file.
//
// Why this is not in the window: a window that reads the filesystem is a window that cannot be
// tested without a screen, and a listing is data with rules worth asserting - folders before files,
// only the extensions this app can read, a count that stops a huge folder from stalling the thread
// that pumps the window.
//
// Why nothing here throws: the walk happens on the thread that feeds the audio ring. A directory that
// is being written while it is walked is a normal thing on a machine with a music folder in it, and
// an exception that escapes into the frame loop is a window that closes mid-note. What was collected
// before the unreadable entry stands, and the frame loop is not where an error gets handled.

#include "boreaz/io/directory.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace boreaz::io {
namespace {

// The two extensions the parser takes. Said here once so the picker and the field cannot disagree
// about what a song is: a file the picker offers is a file `load_song` will accept.
constexpr const char* kSongExtensions[] = {".mid", ".midi"};

// Case, which is not part of a name on this platform, and not part of a sort a player expects.
std::string folding(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

} // namespace

bool reads_as_song(const std::string& name) {
    const std::string lower = folding(name);
    for (const char* extension : kSongExtensions)
        if (lower.size() >= std::string(extension).size() &&
            lower.compare(lower.size() - std::string(extension).size(), std::string::npos,
                          extension) == 0)
            return true;
    return false;
}

std::string current_folder() {
    std::error_code ec;
    const std::filesystem::path here = std::filesystem::current_path(ec);
    return ec ? std::string{} : here.string();
}

std::string parent_folder(const std::string& path) {
    // Lexical and by hand, because `std::filesystem::path::parent_path` has the no-throw form that
    // this libstdc++ does not have, and the whole point of this function is that the frame loop never
    // has to think about it. Both separators, because a path typed at the field on this platform can
    // arrive either way.
    const std::string::size_type mark = path.find_last_of("/\\");
    if (mark == std::string::npos)
        return "."; // a bare name typed at the field: the folder the app is standing in
    return path.substr(0, mark);
}

bool list_folder(const std::string& folder, std::vector<FolderEntry>& into, std::size_t limit,
                 std::size_t& more, std::string& error) {
    into.clear();
    more = 0;

    // A drive root wants its slash spelled out. "C:" is the root of the C: drive to a person who has
    // just pressed Left in C:/songs, and it is the *current* directory on that drive to the
    // filesystem - a thing is_directory refuses. parent_folder answers "C:" for "C:/songs", so this
    // is the edge where a walk up stopped with a message about a folder that plainly exists.
    std::string root = folder;
    if (root.size() == 2 && root[1] == ':')
        root += '/';

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        error = "not a folder: " + folder;
        return false;
    }

    // `skip_permission_denied` because a music folder on Windows holds junctions the user cannot
    // open - "Documents\My Music" is one - and a picker that refused the whole folder over one of
    // them would be a picker with a bug the player cannot fix.
    std::filesystem::directory_iterator walk{
        root, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
        error = "cannot read that folder: " + folder;
        return false;
    }

    // Collected whole first, then cut. A partial listing cut at `limit` before sorting is a
    // different folder's contents on every walk, and a list whose first dozen names change between
    // visits is a list nobody can click on.
    //
    // The walk itself is bounded too. `limit` is about what the player can look at; this is about the
    // thread: a folder of 30 000 files still has to be stepped through to find the three .mid names
    // in it, and every step is a filesystem call on the thread that pumps the window and feeds the
    // audio ring. Past the bound the walk stops and `more` gets one added to it, which the picker
    // reads as "at least this much more" - the honest reading of a count that stopped early.
    constexpr std::size_t kMaxScanned = 4000;
    std::vector<FolderEntry> offered;
    std::size_t scanned = 0;
    bool stopped_early = false;
    const std::filesystem::directory_iterator end;
    for (; walk != end; walk.increment(ec)) {
        if (ec)
            break; // the folder changed under the walk: what is in hand is already sorted below
        if (++scanned > kMaxScanned) {
            stopped_early = true;
            break;
        }
        const std::filesystem::path& what = walk->path();
        std::error_code kind;
        const bool is_folder = std::filesystem::is_directory(what, kind);
        const std::string name = what.filename().string();
        if (!is_folder && !reads_as_song(name))
            continue;
        offered.push_back({name, what.string(), is_folder});
    }

    std::sort(offered.begin(), offered.end(), [](const FolderEntry& a, const FolderEntry& b) {
        if (a.folder != b.folder)
            return a.folder; // folders first: a picker is walked top to bottom, and the walk is down
        const int order = folding(a.name).compare(folding(b.name));
        return order != 0 ? order < 0 : a.name < b.name; // a tie folded is still ordered
    });

    if (offered.size() > limit) {
        more = offered.size() - limit;
        offered.resize(limit);
    }
    if (stopped_early)
        more += 1; // "there was more than this", which is all a bounded walk can promise to say
    into = std::move(offered);
    return true;
}

} // namespace boreaz::io
