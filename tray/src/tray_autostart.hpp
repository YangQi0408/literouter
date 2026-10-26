// "Start literouter when I sign in" — as a user-scoped entry, never a system
// service.
//
// The distinction is the whole point of this file. A system service installs
// outside the user's home, runs before anyone logs in, and needs elevation to
// add or remove; that is appropriate for a server and wrong for a desktop app
// that draws a tray icon, because the icon has to attach to a session. Every
// location below is one the user can delete by hand without sudo:
//
//   Linux    $XDG_CONFIG_HOME/autostart/literouter.desktop   (freedesktop)
//   macOS    ~/Library/LaunchAgents/io.github.yangqi0408.literouter.plist
//   Windows  %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\literouter.lnk
//
// Nothing here is called unless the user asks for it. The app never registers
// itself on first launch: silently adding a login item is exactly the kind of
// thing an installer should not do on someone's behalf.
#pragma once

#include <string>
#include <filesystem>

namespace lrtray {

// The packaged Linux launcher supplies argv[0] even when a private ELF loader
// is /proc/self/exe. Keep autostart and resources pointing at the launcher.
void setExecutablePath(const std::filesystem::path &path);
std::filesystem::path appExecutablePath();

enum class AutostartState {
    // An entry exists and points at this executable.
    Enabled,
    // No entry, or an entry pointing somewhere else.
    Disabled,
    // The platform gave us nowhere to write, or the write failed. The message
    // says which.
    Unsupported,
};

struct AutostartStatus {
    AutostartState state = AutostartState::Disabled;
    std::string path;
    std::string message;
};

// Where the entry would live on this platform, whether or not it exists.
std::string autostartPath();

AutostartStatus queryAutostart();

// Writes the entry for the running executable. Returns the resulting status;
// check `.state` rather than assuming success.
AutostartStatus enableAutostart();

// Removes the entry. Idempotent: removing a missing entry is Disabled, not an
// error.
AutostartStatus disableAutostart();

} // namespace lrtray
