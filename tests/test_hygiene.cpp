// The hygiene gate from the plan's phase-0 exit criteria.
//
// It runs on the source tree itself, so "no secrets, no machine paths, no model names in
// tracked files" is enforced by `ctest` rather than by a reviewer's attention. The rules
// are deliberately narrow: each one is a mistake someone can plausibly make once.

// The gate asks the environment who owns the machine, and MSVC deprecates `getenv` for it. The
// value is copied into a std::string on the line that reads it, so the define is the honest
// answer rather than a per-platform branch around one call. It has to precede the includes.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef BOREAZ_SOURCE_DIR
#error "BOREAZ_SOURCE_DIR must be defined for the hygiene test"
#endif

namespace {

const std::filesystem::path& root() {
    static const std::filesystem::path r{BOREAZ_SOURCE_DIR};
    return r;
}

// Directories that are not ours to police: build output (any naming convention people
// actually use), dependency checkouts, and editor or agent scratch space.
bool skip_dir(const std::string& name) {
    static const std::vector<std::string> kSkip = {".git", ".cache",  "third_party", ".idea",
                                                   ".vs",  ".claude", ".qoder",      "out",
                                                   "dist", "node"};
    for (const auto& s : kSkip) {
        if (name == s || name.rfind(s + "-", 0) == 0) {
            return true;
        }
    }
    return name.rfind("build", 0) == 0 || name.rfind("cmake-build", 0) == 0;
}

// A CMake binary directory by any other name. People configure into `b`, `out`, `_build` or
// their IDE's own folder, and every one of them holds this machine's absolute paths, which is
// precisely what the rules below are looking for.
bool is_build_tree(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / "CMakeCache.txt", ec);
}

// A file is treated as text when its first kilobyte holds no NUL byte. Cheaper and more
// honest than an extension allowlist, which always misses the file someone just added.
bool is_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<char> buffer(1024, 0);
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize n = in.gcount();
    return std::find(buffer.begin(), buffer.begin() + n, '\0') == buffer.begin() + n;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// A login belongs to a person, or to a service. The generic names a CI job or a Windows
// service starts under are not identity, and looking for them would flag ordinary prose.
bool generic_account(const std::string& account) {
    static const std::vector<std::string> kGeneric = {
        "runner",         "containeradministrator", "administrator", "admin", "root",   "git",
        "networkservice", "localservice",           "system",        "ci",    "docker", "vsts",
        "nt authority",
    };
    return std::find(kGeneric.begin(), kGeneric.end(), lower(account)) != kGeneric.end();
}

// Who owns the machine the tree is being checked on. This is the identity a measurement note
// leaks most easily, because it is typed as a filter needle or a folder hop and never looks
// like a path. Under three characters the name matches ordinary text, so it is not looked for.
std::string account_name() {
    for (const char* var : {"USERNAME", "USER", "LOGNAME"}) {
        if (const char* value = std::getenv(var); value != nullptr && *value != '\0') {
            std::string name{value};
            if (name.size() >= 3 && !generic_account(name)) {
                return name;
            }
        }
    }
    return {};
}

// The account name this file is not allowed to contain, or empty when the body is clean.
std::string identity_offender(const std::string& body) {
    const std::string name = account_name();
    return (!name.empty() && lower(body).find(lower(name)) != std::string::npos) ? name
                                                                                 : std::string{};
}

struct Rule {
    std::regex pattern;
    const char* why;
};

const std::vector<Rule>& rules() {
    static const std::vector<Rule> kRules = {
        {std::regex{R"([A-Za-z]:[\\/][Uu]sers[\\/])"}, "machine-absolute path (Windows)"},
        {std::regex{R"([/\\]c[/]Users[/])"}, "machine-absolute path (shell form)"},
        {std::regex{R"(/home/[A-Za-z0-9._-]+)"}, "machine-absolute path (Unix home)"},
        {std::regex{R"(api[_-]?key[ \t]*[=:][ \t]*[A-Za-z0-9])", std::regex::icase},
         "an API key with a value in it"},
        {std::regex{R"(ghp_[A-Za-z0-9]{6,})"}, "a GitHub token"},
    };
    return kRules;
}

std::string rel_path(const std::filesystem::path& path) {
    return std::filesystem::relative(path, root()).generic_string();
}

} // namespace

TEST_CASE("no tracked file contains a machine path or a credential") {
    std::string offenders;
    int files = 0;

    for (auto it = std::filesystem::recursive_directory_iterator(root());
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (skip_dir(it->path().filename().string()) || is_build_tree(it->path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name == "test_hygiene.cpp") {
            continue; // this file necessarily contains the patterns it looks for
        }
        if (name == "NOTICE") {
            continue; // the licence line states the copyright holder's own name on purpose
        }
        if (!is_text(it->path())) {
            continue;
        }
        ++files;
        const std::string body = read_file(it->path());
        for (const auto& rule : rules()) {
            std::smatch match;
            if (std::regex_search(body, match, rule.pattern)) {
                offenders +=
                    "\n  " + rel_path(it->path()) + ": " + rule.why + " -> \"" + match.str() + "\"";
            }
        }
        if (const std::string who = identity_offender(body); !who.empty()) {
            offenders += "\n  " + rel_path(it->path()) + ": this machine's own account name -> \"" +
                         who + "\"";
        }
    }

    INFO("scanned " << files << " text files under " << root().generic_string());
    INFO("offenders:" << offenders);
    CHECK(offenders.empty());
}

TEST_CASE("the account-name rule looks for a person and not for a service login") {
    CHECK_FALSE(generic_account("SomeOne"));
    CHECK(generic_account("runner"));
    CHECK(generic_account("ContainerAdministrator"));
    CHECK(generic_account("NT Authority"));
    CHECK_FALSE(generic_account("SomeOne"));

    const std::string me = account_name();
    INFO("the login this tree is checked under: \"" << me << "\" (empty when it is a generic one)");
    if (me.empty()) {
        return; // a service login carries no identity for the rule to look for
    }
    CHECK_FALSE(identity_offender("cd C:/Users/" + me + "/boreaz").empty());
    CHECK_FALSE(identity_offender("typed " + me + " into the filter").empty());
    CHECK_FALSE(identity_offender(lower(me) + " spelled another way").empty()); // folded
    const std::string other = (me == "SomeOne") ? "SomeTwo" : "SomeOne";
    CHECK(identity_offender("cd C:/Users/" + other + "/boreaz").empty());
    CHECK(identity_offender("the job ran as runner and remembered nothing").empty());
}

TEST_CASE("every BOREAZ_ key in .env.example has an empty value") {
    const std::filesystem::path example = root() / ".env.example";
    REQUIRE(std::filesystem::exists(example));

    const std::regex key{R"(^(BOREAZ_[A-Z0-9_]+)=(.*)$)"};
    std::smatch match;
    std::istringstream stream(read_file(example));
    std::string line;
    std::string filled;
    int keys = 0;

    while (std::getline(stream, line)) {
        if (!std::regex_search(line, match, key)) {
            continue;
        }
        ++keys;
        if (match[2].matched && !match[2].str().empty()) {
            filled += "\n  " + match[1].str() + "=" + match[2].str();
        }
    }

    INFO("keys found: " << keys);
    INFO("keys with committed values:" << filled);
    CHECK(keys > 0);
    CHECK(filled.empty());
}

TEST_CASE("the repo cannot forget its own guardrails") {
    // .env must stay ignored, or a developer's real device names and paths get published.
    const std::string gitignore = read_file(root() / ".gitignore");
    CHECK(gitignore.find(".env") != std::string::npos);
    CHECK(gitignore.find("\n.env") != std::string::npos); // anchored, not ".environment"

    // Attribution for the sampled piano is a licence condition, not a courtesy.
    const std::string notice = read_file(root() / "NOTICE");
    CHECK(notice.find("Required Notice:") != std::string::npos);
    CHECK(notice.find("Salamander") != std::string::npos);
    CHECK(notice.find("Creative Commons Attribution") != std::string::npos);

    const std::string license = read_file(root() / "LICENSE");
    CHECK(license.find("PolyForm Noncommercial License 1.0.0") != std::string::npos);

    // A .env present in the tree means someone is about to commit one.
    CHECK(!std::filesystem::exists(root() / ".env"));
}

TEST_CASE("the synthesis core never includes a device, a file, or a window") {
    // Plan 3's dependency direction, checked instead of hoped for. `dsp` and `engine` are the
    // leaves that must never learn about miniaudio, a GUI toolkit, the Windows API, files, or
    // the sample/SFZ/midi layers: that is what keeps the engine testable headless and what stops
    // an I/O call creeping into the audio callback. CMakeLists.txt refuses to *link* those into
    // boreaz_engine; this catches the include that would come before the link.
    static const std::regex include_re{R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])"};
    static const std::vector<std::string> kForbidden = {
        "miniaudio", "imgui",     "glfw",       "GLFW",        "d3d11",
        "dxgi",      "windows.h", "fstream",    "iostream",    "sfz",
        "samples/",  "midifile",  "boreaz/io/", "boreaz/gui/", "boreaz/seq/"};
    static const std::vector<std::string> kDirs = {"include/boreaz/dsp", "include/boreaz/engine",
                                                   "src/dsp", "src/engine"};

    std::string offenders;
    int files = 0;
    for (const auto& dir : kDirs) {
        const auto base = root() / dir;
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string extension = entry.path().extension().string();
            if (extension != ".h" && extension != ".cpp") {
                continue;
            }
            ++files;
            std::istringstream stream(read_file(entry.path()));
            std::string line;
            std::smatch match;
            while (std::getline(stream, line)) {
                if (!std::regex_search(line, match, include_re)) {
                    continue;
                }
                for (const auto& token : kForbidden) {
                    if (match[1].str().find(token) != std::string::npos) {
                        offenders += "\n  " + rel_path(entry.path()) + ": " + line;
                    }
                }
            }
        }
    }

    INFO("files checked: " << files);
    INFO("offenders:" << offenders);
    CHECK(files > 0);
    CHECK(offenders.empty());
}

TEST_CASE("the io and midifile layers print nothing and wait for nothing") {
    // From phase 3 on, the audio callback lives in this layer. Printing is the classic way a
    // callback becomes a glitch - a console write can take milliseconds and can block on a
    // terminal nobody is reading - and sleeping in it is the same mistake with a different name.
    // The code comments ask for this; here it is checked. Reports belong to main().
    //
    // midifile is in scope for the same reason even though no audio callback runs in it: it feeds
    // that callback, and a library that prints to a console the GUI application does not have is
    // a report that goes nowhere while the notes keep vanishing.
    static const std::vector<std::regex> kForbidden = {
        std::regex{R"(\bprintf\s*\()"},  std::regex{R"(\bputs\s*\()"},
        std::regex{R"(\bcout\s*<<)"},    std::regex{R"(\bcerr\s*<<)"},
        std::regex{R"(std::exit\s*\()"}, std::regex{R"(std::abort\s*\()"},
        std::regex{R"(sleep_for\s*\()"}, std::regex{R"(\bSleep\s*\()"},
    };
    static const std::vector<std::string> kDirs = {"include/boreaz/io", "src/io",
                                                   "include/boreaz/midifile", "src/midifile"};

    std::string offenders;
    int files = 0;
    for (const auto& dir : kDirs) {
        const auto base = root() / dir;
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string extension = entry.path().extension().string();
            if (extension != ".h" && extension != ".cpp") {
                continue;
            }
            ++files;
            std::istringstream stream(read_file(entry.path()));
            std::string line;
            int line_number = 0;
            while (std::getline(stream, line)) {
                ++line_number;
                for (const auto& forbidden : kForbidden) {
                    if (std::regex_search(line, forbidden)) {
                        offenders += "\n  " + rel_path(entry.path()) + ":" +
                                     std::to_string(line_number) + ": " + line;
                    }
                }
            }
        }
    }

    INFO("files checked: " << files);
    INFO("offenders:" << offenders);
    CHECK(files > 0);
    CHECK(offenders.empty());
}
