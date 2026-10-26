// The three implementations of "start at sign-in", one per platform, each
// confined to the block below.
//
// They share a shape: a fixed path derived from the environment, a small text
// file, and an existence check. None of them needs elevation, and none of them
// touches anything outside the user's profile — see the header for why that
// line is drawn where it is.
#include "tray_autostart.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <limits.h>
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#  include <limits.h>
#endif

namespace lrtray {
namespace {
std::filesystem::path g_executable;

std::filesystem::path executablePath() {
    if (!g_executable.empty()) return g_executable;
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4] = {0};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH * 4);
    if (length == 0) {
        return {};
    }
    return std::filesystem::path{std::wstring{buffer, length}};
#elif defined(__APPLE__)
    // NSGetExecutablePath gives the path the process was launched with, which
    // inside a .app is Contents/MacOS/literouter. The autostart entry should
    // name the bundle, not the inner binary, so the caller strips it — see
    // bundlePathFromExecutable below.
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return std::filesystem::path{buffer};
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

#if defined(__APPLE__)
// ~/Applications/literouter.app/Contents/MacOS/literouter -> ~/Applications/literouter.app
// LaunchServices wants the bundle: `open -a` on the inner binary would start a
// second copy outside the bundle's identity, which is the problem the bundle
// exists to avoid.
std::filesystem::path bundlePathFromExecutable(const std::filesystem::path &executable) {
    auto candidate = executable;
    for (int depth = 0; depth < 4; ++depth) {
        candidate = candidate.parent_path();
        if (candidate.empty()) {
            return {};
        }
        if (candidate.extension() == ".app") {
            return candidate;
        }
    }
    return {};
}
#endif

std::string homeDir() {
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
#if defined(_WIN32)
    if (const char *profile = std::getenv("USERPROFILE");
        profile != nullptr && *profile != '\0') {
        return profile;
    }
#endif
    return {};
}

bool writeFile(const std::filesystem::path &path, const std::string &contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }
    out << contents;
    out.close();
    return static_cast<bool>(out);
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string xmlEscape(const std::string &text) {
    std::string escaped;
    for (const char c : text) {
        if (c == '&') escaped += "&amp;";
        else if (c == '<') escaped += "&lt;";
        else if (c == '>') escaped += "&gt;";
        else if (c == '"') escaped += "&quot;";
        else escaped += c;
    }
    return escaped;
}

} // namespace

void setExecutablePath(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        g_executable = std::filesystem::absolute(path, ec);
}

std::filesystem::path appExecutablePath() { return executablePath(); }

// ── Linux: a freedesktop autostart entry ─────────────────────────────────────
#if !defined(_WIN32) && !defined(__APPLE__)

std::string autostartPath() {
    std::string base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        base = xdg;
    } else if (const std::string home = homeDir(); !home.empty()) {
        base = home + "/.config";
    } else {
        return {};
    }
    return base + "/autostart/literouter.desktop";
}

AutostartStatus queryAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "no XDG_CONFIG_HOME and no HOME"};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return {AutostartState::Disabled, path, {}};
    }
    // The entry exists; it counts as enabled only if it names this executable.
    // A stale entry from a previous install location should read as Disabled so
    // the user is offered a rewrite rather than being told it is already on.
    const std::string contents = readFile(path);
    const std::string self = executablePath().string();
    if (!self.empty() && contents.find(self) != std::string::npos) {
        return {AutostartState::Enabled, path, {}};
    }
    return {AutostartState::Disabled, path, "an autostart entry exists but points elsewhere"};
}

AutostartStatus enableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "no XDG_CONFIG_HOME and no HOME"};
    }
    const std::string self = executablePath().string();
    if (self.empty()) {
        return {AutostartState::Unsupported, path, "cannot determine this executable's path"};
    }
    // Exec is quoted because a path with a space would otherwise be split into
    // two arguments. X-GNOME-Autostart-enabled is what GNOME reads; the other
    // desktops ignore it and go by Hidden.
    const std::string contents =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=literouter\n"
        "Comment=Local AI relay gateway\n"
        "Exec=\"" + self + "\"\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Hidden=false\n";
    if (!writeFile(path, contents)) {
        return {AutostartState::Unsupported, path, "cannot write the autostart entry"};
    }
    return {AutostartState::Enabled, path, {}};
}

AutostartStatus disableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "no XDG_CONFIG_HOME and no HOME"};
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return {AutostartState::Unsupported, path, ec.message()};
    }
    return {AutostartState::Disabled, path, {}};
}

// ── macOS: a per-user LaunchAgent ────────────────────────────────────────────
#elif defined(__APPLE__)

namespace {
// LaunchAgents load from this directory at login, for this user only. The
// system-wide /Library/LaunchAgents and /Library/LaunchDaemons are the ones
// that would need sudo, and are deliberately not used.
std::string launchAgentLabel() { return "io.github.yangqi0408.literouter"; }
} // namespace

std::string autostartPath() {
    const std::string home = homeDir();
    if (home.empty()) {
        return {};
    }
    return home + "/Library/LaunchAgents/" + launchAgentLabel() + ".plist";
}

AutostartStatus queryAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "HOME is not set"};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return {AutostartState::Disabled, path, {}};
    }
    return {AutostartState::Enabled, path, {}};
}

AutostartStatus enableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "HOME is not set"};
    }
    const std::filesystem::path executable = executablePath();
    if (executable.empty()) {
        return {AutostartState::Unsupported, path, "cannot determine this executable's path"};
    }
    // Name the bundle when there is one: `open -a` on the inner binary would
    // start a copy that LaunchServices does not associate with the app.
    const std::filesystem::path bundle = bundlePathFromExecutable(executable);
    const std::string target = xmlEscape(bundle.empty() ? executable.string() : bundle.string());
    const std::string arguments = bundle.empty()
        ? "        <string>" + target + "</string>\n"
        : "        <string>/usr/bin/open</string>\n"
          "        <string>-a</string>\n"
          "        <string>" + target + "</string>\n";
    const std::string contents =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key><string>" + launchAgentLabel() + "</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        + arguments +
        "    </array>\n"
        "    <key>RunAtLoad</key><true/>\n"
        "    <key>KeepAlive</key><false/>\n"
        "    <key>ProcessType</key><string>Interactive</string>\n"
        "</dict>\n"
        "</plist>\n";
    if (!writeFile(path, contents)) {
        return {AutostartState::Unsupported, path, "cannot write the LaunchAgent"};
    }
    return {AutostartState::Enabled, path, {}};
}

AutostartStatus disableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "HOME is not set"};
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return {AutostartState::Unsupported, path, ec.message()};
    }
    return {AutostartState::Disabled, path, {}};
}

// ── Windows: a shortcut in the per-user Startup folder ───────────────────────
#else

namespace {

// %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
//
// The Startup folder rather than a scheduled task or a Run key, for three
// reasons that all point the same way: it is per-user (no elevation), it is
// visible in Explorer so the user can delete the entry without a tool, and it
// is what Task Manager's Startup tab lists, so turning it off is one click.
// A scheduled task is what the CLI's `--service` path uses, and that is a
// deliberate, different choice for a headless gateway.
std::filesystem::path startupFolder() {
    PWSTR raw = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &raw) != S_OK) {
        return {};
    }
    std::filesystem::path folder{raw};
    ::CoTaskMemFree(raw);
    return folder;
}

} // namespace

std::string autostartPath() {
    const std::filesystem::path folder = startupFolder();
    if (folder.empty()) {
        return {};
    }
    return (folder / "literouter.lnk").string();
}

AutostartStatus queryAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "cannot resolve the Startup folder"};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return {AutostartState::Disabled, path, {}};
    }
    return {AutostartState::Enabled, path, {}};
}

AutostartStatus enableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "cannot resolve the Startup folder"};
    }
    // A .lnk needs the shell's COM interface; there is no text-file shortcut.
    // That is the one place this platform's implementation is heavier than the
    // other two, and it is why the file lives in its own translation unit.
    const std::filesystem::path self = executablePath();
    if (self.empty()) {
        return {AutostartState::Unsupported, path, "cannot determine this executable's path"};
    }
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return {AutostartState::Unsupported, path, "cannot initialize the shell link apartment"};
    }
    struct Apartment {
        bool owns;
        ~Apartment() { if (owns) ::CoUninitialize(); }
    } apartment{SUCCEEDED(initialized)};
    IShellLinkW *link = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void **>(&link)))) {
        return {AutostartState::Unsupported, path, "cannot create a shell link"};
    }
    link->SetPath(self.c_str());
    link->SetDescription(L"literouter");
    link->SetWorkingDirectory(self.parent_path().c_str());

    IPersistFile *file = nullptr;
    AutostartStatus status{AutostartState::Unsupported, path, "cannot persist the shell link"};
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        if (SUCCEEDED(file->Save(std::filesystem::path{path}.c_str(), TRUE))) {
            status = {AutostartState::Enabled, path, {}};
        }
        file->Release();
    }
    link->Release();
    return status;
}

AutostartStatus disableAutostart() {
    const std::string path = autostartPath();
    if (path.empty()) {
        return {AutostartState::Unsupported, {}, "cannot resolve the Startup folder"};
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return {AutostartState::Unsupported, path, ec.message()};
    }
    return {AutostartState::Disabled, path, {}};
}

#endif

} // namespace lrtray
