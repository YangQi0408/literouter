// String, path, formatting and secret helpers. No sockets here — this unit is
// the part of the engine a test can call without a network or a config file.
module literouter.core;

import std;

namespace literouter {

namespace {

// Where a variable reference's name ends: at '}' for the braced form, or at
// the first byte that cannot be part of an identifier for the bare form.
bool isNameByte(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

std::optional<std::string> envValue(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::string key{name};
    if (const char *raw = std::getenv(key.c_str()); raw != nullptr) {
        return std::string{raw};
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path userHome() {
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home};
    }
#ifdef _WIN32
    if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return std::filesystem::path{profile};
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path defaultConfigDir() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "literouter";
    }
#ifdef _WIN32
    // On Windows, if a config already exists in %USERPROFILE%\.config\literouter,
    // continue using it for backward compatibility.
    const auto dotConfig = userHome() / ".config" / "literouter";
    std::error_code ec;
    if (std::filesystem::exists(dotConfig / "config.json", ec)) {
        return dotConfig;
    }
    // Otherwise, standard Windows configuration directory is %APPDATA%\literouter
    if (const char *appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path{appdata} / "literouter";
    }
#endif
    return userHome() / ".config" / "literouter";
}

std::filesystem::path defaultConfigPath() {
    if (const char *env = std::getenv("LITEROUTER_CONFIG"); env != nullptr && *env != '\0') {
        return std::filesystem::path{env};
    }
    return defaultConfigDir() / "config.json";
}

std::filesystem::path defaultStateDir() {
    if (const char *env = std::getenv("LITEROUTER_STATE_DIR"); env != nullptr && *env != '\0') {
        return std::filesystem::path{env};
    }
    if (const char *xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "literouter";
    }
#ifdef _WIN32
    // On Windows, standard local application state directory is %LOCALAPPDATA%\literouter
    if (const char *local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        return std::filesystem::path{local} / "literouter";
    }
#endif
    return userHome() / ".local" / "state" / "literouter";
}

std::filesystem::path defaultTelemetryPath(int port) {
    return defaultStateDir() / std::format("telemetry-{}.json", port);
}

std::filesystem::path defaultPidPath(int port) {
    return defaultStateDir() / std::format("literouter-{}.pid", port);
}

std::filesystem::path resolveCaBundle() {
    const auto fromEnv = [](const char *name) -> std::filesystem::path {
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return {};
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(value, ec)) {
            return std::filesystem::path{value};
        }
        return {};
    };

    for (const char *name : {"LITEROUTER_CA_BUNDLE", "SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
        if (auto path = fromEnv(name); !path.empty()) {
            return path;
        }
    }

#ifdef _WIN32
    // On Windows, scan common locations where Git, curl, or OpenSSL bundles are installed.
    const auto fromEnvDir = [](const char *envName, std::string_view subpath) -> std::filesystem::path {
        const char *base = std::getenv(envName);
        if (base == nullptr || *base == '\0') {
            return {};
        }
        auto p = std::filesystem::path{base} / subpath;
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            return p;
        }
        return {};
    };

    for (const auto &[envName, subpath] : {
             std::pair{"ProgramFiles", "Git/usr/ssl/certs/ca-bundle.crt"},
             std::pair{"ProgramFiles(x86)", "Git/usr/ssl/certs/ca-bundle.crt"},
             std::pair{"LocalAppData", "Programs/Git/usr/ssl/certs/ca-bundle.crt"},
             std::pair{"ProgramData", "curl/bin/curl-ca-bundle.crt"},
             std::pair{"SystemRoot", "System32/curl-ca-bundle.crt"},
         }) {
        if (auto p = fromEnvDir(envName, subpath); !p.empty()) {
            return p;
        }
    }
#else
    // Ordered by how common the path is, not by distro: a Debian-family bundle
    // is the likeliest hit on any machine that also has an /etc/pki copy.
    static constexpr std::array<std::string_view, 9> candidates{
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Arch, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",     // RHEL, Fedora, CentOS
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // macOS, BSD, Alpine
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
        "/etc/certs/ca-certificates.crt",
        "/usr/local/etc/openssl/cert.pem",        // Homebrew Intel
    };
    std::error_code ec;
    for (const auto candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::path{candidate};
        }
    }
#endif
    return {};
}

std::string caBundleSummary() {
    const auto path = resolveCaBundle();
    return path.empty() ? std::string{} : path.string();
}

// ── secrets ──────────────────────────────────────────────────────────────────

std::string resolveSecret(std::string_view raw) {
    const std::string text = trim(raw);
    if (text.empty()) {
        return {};
    }
    const bool braced = text.size() >= 3 && text[0] == '$' && text[1] == '{';
    const bool bare = text.size() >= 2 && text[0] == '$' && text[1] != '{';
    if (!braced && !bare) {
        return text;
    }

    std::string_view inner;
    if (braced) {
        const auto close = text.find('}', 2);
        if (close == std::string::npos) {
            // An unterminated "${" is a literal, not a hole to guess at.
            return text;
        }
        inner = std::string_view{text}.substr(2, close - 2);
    } else {
        std::size_t end = 1;
        while (end < text.size() && isNameByte(text[end])) {
            ++end;
        }
        inner = std::string_view{text}.substr(1, end - 1);
    }

    // `${NAME:-fallback}`
    std::string_view name = inner;
    std::string_view fallback;
    bool hasFallback = false;
    if (const auto sep = inner.find(":-"); sep != std::string_view::npos) {
        name = inner.substr(0, sep);
        fallback = inner.substr(sep + 2);
        hasFallback = true;
    }

    if (auto value = envValue(name)) {
        return *value;
    }
    return hasFallback ? std::string{fallback} : std::string{};
}

bool isSecretReference(std::string_view raw) {
    const std::string text = trim(raw);
    return text.size() >= 2 && text[0] == '$';
}

std::string secretReferenceName(std::string_view raw) {
    const std::string text = trim(raw);
    if (text.size() < 2 || text[0] != '$') {
        return {};
    }
    if (text[1] == '{') {
        const auto close = text.find('}', 2);
        if (close == std::string::npos) {
            return {};
        }
        std::string name{std::string_view{text}.substr(2, close - 2)};
        if (const auto sep = name.find(":-"); sep != std::string::npos) {
            name.resize(sep);
        }
        return name;
    }
    std::size_t end = 1;
    while (end < text.size() && isNameByte(text[end])) {
        ++end;
    }
    return text.substr(1, end - 1);
}

std::string maskSecret(std::string_view value) {
    if (value.empty()) {
        return "(none)";
    }
    if (value.size() <= 8) {
        return std::string(value.size(), '*');
    }
    // Showing four bytes at each end is enough to tell two keys apart in a
    // list without handing a shoulder-surfer anything useful.
    return std::format("{}{}…{}", std::string_view{value}.substr(0, 4),
                       std::string(4, '*'),
                       std::string_view{value}.substr(value.size() - 4));
}

// ── base urls ────────────────────────────────────────────────────────────────

bool splitBaseUrl(std::string_view base_url, std::string &root, std::string &prefix,
                  std::string &scheme) {
    root.clear();
    prefix.clear();
    scheme.clear();

    // A view into a temporary would dangle the moment the full expression
    // ends, so the trimmed copy is materialised first.
    const std::string trimmed = trim(base_url);
    std::string_view rest{trimmed};
    if (rest.empty()) {
        return false;
    }

    if (const auto sep = rest.find("://"); sep != std::string_view::npos) {
        scheme = toLower(rest.substr(0, sep));
        rest = rest.substr(sep + 3);
    } else {
        scheme = "https";
    }
    if (scheme != "http" && scheme != "https") {
        return false;
    }

    std::string_view authority = rest;
    std::string_view path;
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        authority = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    if (authority.empty()) {
        return false;
    }

    root = std::format("{}://{}", scheme, authority);
    prefix = std::string{path};
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    if (!prefix.empty() && prefix.front() != '/') {
        prefix.insert(prefix.begin(), '/');
    }
    return true;
}

std::string joinPath(std::string_view prefix, std::string_view path) {
    std::string out{prefix};
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    if (path.empty()) {
        return out.empty() ? "/" : out;
    }
    if (path.front() != '/') {
        out.push_back('/');
        out.append(path);
    } else {
        // If out already ends with the leading segment of path, strip it from path.
        // E.g. out ends with "/v1" and path starts with "/v1/" or is "/v1".
        // Or out ends with "/v1beta" and path starts with "/v1beta/".
        const auto next_slash = path.find('/', 1);
        const std::string_view first_seg = (next_slash == std::string_view::npos)
                                               ? path
                                               : path.substr(0, next_slash);
        if (out == first_seg) {
            path = (next_slash == std::string_view::npos) ? std::string_view{} : path.substr(next_slash);
        }
        out.append(path);
    }
    return out.empty() ? "/" : out;
}

// ── small string utilities ───────────────────────────────────────────────────

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::string toLower(std::string_view text) {
    std::string out{text};
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string truncateUtf8(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string{text};
    }
    std::size_t cut = limit;
    // Walk back off any continuation byte so the result is still valid UTF-8.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    // ...and off a lead byte whose sequence was truncated.
    if (cut > 0) {
        const unsigned char lead = static_cast<unsigned char>(text[cut - 1]);
        std::size_t expected = 1;
        if ((lead & 0xE0) == 0xC0) {
            expected = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            expected = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            expected = 4;
        }
        if (cut - 1 + expected > text.size()) {
            cut -= 1;
        }
    }
    return std::format("{}…", text.substr(0, cut));
}

std::string humanCount(std::uint64_t value) {
    if (value < 1000) {
        return std::to_string(value);
    }
    // The unit belongs to the number of divisions already performed, not to the
    // next one: 1000 is "1.0k", not "1.0M". The loop is `do`-`while` because the
    // early return above guarantees at least one division has to happen.
    //
    // The threshold is 999.5 rather than 1000 because the printed form rounds:
    // stopping at "999.999k" would print as "1000k", which is exactly the
    // discontinuity the extra division exists to remove.
    static constexpr std::array<std::string_view, 4> units{"k", "M", "G", "T"};
    double scaled = static_cast<double>(value);
    std::size_t divisions = 0;
    do {
        scaled /= 1000.0;
        ++divisions;
    } while (scaled >= 999.5 && divisions < units.size());

    const std::string_view unit = units[divisions - 1];
    return scaled < 10.0 ? std::format("{:.1f}{}", scaled, unit)
                         : std::format("{:.0f}{}", scaled, unit);
}

std::string humanMillis(double ms) {
    if (ms <= 0.0) {
        return "—";
    }
    if (ms < 1000.0) {
        return std::format("{:.0f}ms", ms);
    }
    return std::format("{:.2f}s", ms / 1000.0);
}

std::string humanBytes(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{}B", bytes);
    }
    // Same shape as humanCount, in powers of 1024 and with the B suffix carried
    // on the unit rather than on the number.
    static constexpr std::array<std::string_view, 4> units{"K", "M", "G", "T"};
    double scaled = static_cast<double>(bytes);
    std::size_t divisions = 0;
    do {
        scaled /= 1024.0;
        ++divisions;
    } while (scaled >= 1023.5 && divisions < units.size());

    const std::string_view unit = units[divisions - 1];
    return scaled < 10.0 ? std::format("{:.1f}{}B", scaled, unit)
                         : std::format("{:.0f}{}B", scaled, unit);
}

std::string humanDuration(double seconds) {
    if (seconds < 0.0) {
        return "—";
    }
    const auto total = static_cast<long long>(seconds);
    const long long days = total / 86400;
    const long long hours = (total % 86400) / 3600;
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    if (days > 0) {
        return std::format("{}d {}h", days, hours);
    }
    if (hours > 0) {
        return std::format("{}h {}m", hours, minutes);
    }
    if (minutes > 0) {
        return std::format("{}m {}s", minutes, secs);
    }
    return std::format("{}s", secs);
}

// Same shape as humanDuration, minus the compaction: a total that a console
// repaints every second has to end in seconds, or a server up for an hour looks
// like a clock that stopped between minute changes.
std::string humanUptime(double seconds) {
    if (seconds < 0.0) {
        return "—";
    }
    const auto total = static_cast<long long>(seconds);
    const long long days = total / 86400;
    const long long hours = (total % 86400) / 3600;
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    if (days > 0) {
        return std::format("{}d {}h {}m {}s", days, hours, minutes, secs);
    }
    if (hours > 0) {
        return std::format("{}h {}m {}s", hours, minutes, secs);
    }
    if (minutes > 0) {
        return std::format("{}m {}s", minutes, secs);
    }
    return std::format("{}s", secs);
}

std::string redactSecrets(std::string_view text) {
    if (text.empty()) {
        return std::string{text};
    }
    std::string out{text};

    // One pass per rule. The order matters only in that the private-key block
    // goes first: the value rules would otherwise chew it into fragments.
    // ECMAScript grammar, so case-insensitivity is a construction flag rather
    // than the `(?i)` inline form some engines accept — writing `(?i)` here
    // would make the pattern match the literal text "(?i)" instead.
    using Flags = std::regex_constants::syntax_option_type;
    struct Rule {
        const char *pattern;
        const char *replacement;
        Flags flags = std::regex_constants::ECMAScript;
    };
    const Rule rules[] = {
        // A PEM block, whatever it wraps.
        {R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z0-9 ]*PRIVATE KEY-----)",
         "[redacted private key]"},
        // A bearer token, in a header line or pasted into a prompt.
        {R"((bearer\s+)[A-Za-z0-9._~+/=-]{16,})", "$1[redacted]",
         std::regex_constants::ECMAScript | std::regex_constants::icase},
        // Known key prefixes. The prefix survives, so a reader can tell what it
        // was without being able to use it.
        {R"(\b(sk-ant-|sk-proj-|sk-|AIza|ghp_|gho_|github_pat_|xoxb-|xoxp-|AKIA|glpat-)[A-Za-z0-9_-]{16,})",
         "$1[redacted]"},
        // A JWT: three base64url segments, the first of which decodes to `{"`.
        {R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})", "[redacted jwt]"},
        // A labelled value — `api_key: abc…`, `"token":"abc…"` — where the value
        // is long enough to be a credential rather than a word.
        {R"(((?:api[_-]?key|access[_-]?token|auth[_-]?token|secret|password)\"?\s*[:=]\s*\"?)([A-Za-z0-9._~+/-]{16,}))",
         "$1[redacted]", std::regex_constants::ECMAScript | std::regex_constants::icase},
    };
    for (const auto &rule : rules) {
        try {
            const std::regex pattern{rule.pattern, rule.flags};
            out = std::regex_replace(out, pattern, rule.replacement);
        } catch (const std::regex_error &) {
            // A pattern this build cannot compile is a bug, but it must not take
            // a request down with it: the body is logged unredacted instead.
            continue;
        }
    }
    return out;
}

std::string hexId(std::size_t bytes) {
    // std::random_device on some libc++ builds is deterministic; mixing in the
    // clock and the thread id costs nothing and makes a collision unlikely
    // even where the entropy source is weak.
    static std::atomic<std::uint64_t> counter{0};
    const auto tick = static_cast<std::uint64_t>(nowUnix() * 1'000'000.0);
    const auto extra = (static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())) << 17) ^
                       counter.fetch_add(1, std::memory_order_relaxed);
    std::mt19937_64 rng{std::random_device{}() ^ tick ^ extra};
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto byte = static_cast<unsigned>(rng() & 0xFF);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

double nowUnix() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ── config model helpers ─────────────────────────────────────────────────────

const ProviderConfig *AppConfig::provider(std::string_view id) const {
    for (const auto &entry : providers) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

ProviderConfig *AppConfig::provider(std::string_view id) {
    for (auto &entry : providers) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const RouteConfig *AppConfig::route(std::string_view model) const {
    for (const auto &entry : routes) {
        if (entry.model == model) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::string> AppConfig::routedModels() const {
    std::vector<std::string> out;
    out.reserve(routes.size());
    for (const auto &entry : routes) {
        if (entry.enabled && !entry.model.empty()) {
            out.push_back(entry.model);
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<std::string> AppConfig::logicalModels() const {
    return routedModels();
}

std::vector<std::string> AppConfig::allModels() const {
    std::vector<std::string> out = routedModels();
    for (const auto &entry : providers) {
        if (!entry.enabled) {
            continue;
        }
        for (const auto &model : entry.models) {
            if (std::ranges::find(out, model) == out.end()) {
                out.push_back(model);
            }
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

// ── security ─────────────────────────────────────────────────────────────────

bool secureEquals(std::string_view a, std::string_view b) {
    // Length is not secret here (both sides are configuration), but a
    // byte-by-byte loop that does not stop at the first mismatch is still the
    // right shape for the comparison a key check does.
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string extractApiKey(const std::map<std::string, std::string, std::less<>> &headers) {
    if (const auto it = headers.find("authorization"); it != headers.end()) {
        const std::string trimmed = trim(it->second);
        std::string_view value{trimmed};
        if (value.size() > 7 && toLower(value.substr(0, 7)) == "bearer ") {
            return trim(value.substr(7));
        }
        if (!value.empty()) {
            return std::string{value};
        }
    }
    if (const auto it = headers.find("x-api-key"); it != headers.end()) {
        return trim(it->second);
    }
    return {};
}

} // namespace literouter
