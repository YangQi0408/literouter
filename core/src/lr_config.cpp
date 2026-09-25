// ConfigStore: where the model lives, how it is validated, and how it is
// written back without a half-written file ever being observable.
module;

#include <openssl/ssl.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <aclapi.h>
// SetNamedSecurityInfoW and friends live in advapi32, which is not in the
// default link line. The pragma is what cpp-httplib itself does for ws2_32
// (httplib.h), so this stays the one mechanism the project uses for a Windows
// import library, and it works under clang-cl as well as cl.exe.
#  if defined(_MSC_VER)
#    pragma comment(lib, "advapi32.lib")
#  endif
#endif

module literouter.core;

import std;

namespace literouter {

namespace {

#ifdef _WIN32
// The Windows equivalent of the `0600` above, and the same intent: a config
// with a literal key in it should be readable by its owner and by nobody else.
//
// `std::filesystem::permissions` cannot express this — it maps onto the
// read-only attribute, not onto an ACL — so this replaces the file's DACL with
// a single entry for the current user. `%APPDATA%` normally inherits a
// user-only ACL already, so the common case is a no-op that costs one call;
// the case this exists for is a config placed somewhere shared by hand, or a
// directory whose inherited ACL is wider than the default.
//
// Deliberately best-effort: a failure here means the file is left with
// whatever ACL it had, which is exactly the behaviour before this existed, and
// it must not turn a successful save into a reported error.
void restrictToOwner(const std::filesystem::path &path) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (size == 0 || GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == FALSE) {
        CloseHandle(token);
        return;
    }
    CloseHandle(token);

    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
    // An explicit ACL with no inheritance: one ACE, full control, the owner.
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);

    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
        return;
    }
    SetNamedSecurityInfoW(const_cast<LPWSTR>(path.wstring().c_str()), SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
}
#endif

} // namespace

AppConfig ConfigStore::seedDefault() {
    AppConfig config;

    // A seeded relay rather than an empty list: `literouter config init` and a
    // first `serve` should show a filled-in example the user edits, instead of
    // an empty array they have to guess the shape of. The key is a reference,
    // so the seed is safe to commit, screenshot or sync.
    ProviderConfig openai;
    openai.id = "openai";
    openai.name = "OpenAI";
    openai.base_url = "https://api.openai.com/v1";
    openai.api_key = "${OPENAI_API_KEY}";
    openai.priority = 10;
    openai.models = {"gpt-4o", "gpt-4o-mini", "text-embedding-3-small"};
    openai.note = "Official endpoint. Set OPENAI_API_KEY, or replace this entry.";
    config.providers.push_back(std::move(openai));

    ProviderConfig relay;
    relay.id = "relay";
    relay.name = "Backup relay";
    relay.base_url = "https://example-relay.invalid/v1";
    relay.api_key = "${RELAY_API_KEY}";
    relay.priority = 50;
    relay.enabled = false;
    relay.models = {"gpt-4o"};
    relay.note = "Disabled placeholder — point it at a real relay and enable it.";
    config.providers.push_back(std::move(relay));

    // The route is what makes failover visible: one logical name, two hops,
    // tried in order.
    RouteConfig route;
    route.model = "gpt-4o";
    route.targets.push_back(RouteTarget{.provider = "openai", .model = {}});
    route.targets.push_back(RouteTarget{.provider = "relay", .model = {}});
    config.routes.push_back(std::move(route));

    return config;
}

std::expected<ConfigStore, std::string> ConfigStore::load(const std::filesystem::path &path) {
    ConfigStore store;
    store.path_ = path;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Absent is a valid state, not an error: the caller decides whether to
        // seed, prompt, or run with the example.
        store.config_ = seedDefault();
        store.exists_ = false;
        return store;
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected(std::format("cannot read {}", pathToUtf8(path)));
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();

    // Walked onto this build's schema before it is used, so an older file keeps
    // loading and the operator is told what was normalised instead of being
    // handed a config whose behaviour differs from its text. The file itself is
    // left alone: loading must not rewrite what someone is editing, and
    // `literouter config migrate` is the explicit way to write it back.
    std::string rewritten;
    auto migration = migrateConfigJson(text, rewritten);
    if (!migration) {
        return std::unexpected(std::format("{}: {}", pathToUtf8(path), migration.error()));
    }
    if (auto parsed = appConfigFromJson(rewritten); !parsed) {
        return std::unexpected(std::format("{}: {}", pathToUtf8(path), parsed.error()));
    } else {
        store.config_ = std::move(*parsed);
    }
    store.migration_ = std::move(*migration);

    store.exists_ = true;
    if (std::filesystem::exists(path, ec)) {
        store.mtime = std::filesystem::last_write_time(path, ec);
    }
    return store;
}

std::expected<ConfigStore, std::string> ConfigStore::loadDefault() {
    return load(defaultConfigPath());
}

std::expected<ConfigStore, std::string> ConfigStore::loadOrSeed(
    const std::filesystem::path &path) {
    auto loaded = load(path);
    if (loaded) {
        return loaded;
    }
    // Kept as a distinct entry point because "the file is broken" and "the file
    // is missing" want different behaviour in a front end: `serve` should
    // refuse to run against a corrupted config, while the console should open
    // and offer to repair it.
    ConfigStore store;
    store.path_ = path;
    store.config_ = seedDefault();
    store.exists_ = false;
    store.loadError_ = loaded.error();
    return store;
}

std::expected<void, std::string> ConfigStore::fromJson(std::string_view text) {
    auto parsed = appConfigFromJson(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    config_ = std::move(*parsed);
    return {};
}

// Replaces `destination` with the already-written `temp` without ever leaving
// the operator with no config file. Called only after the atomic
// `rename` the caller tries first has been refused.
//
// That first attempt is the one every ordinary save takes: a rename within one
// directory replaces in place and is atomic. It is refused when the destination
// cannot be replaced — on Windows, `MoveFileEx` with replace-existing fails
// while the file is open with sharing flags that forbid the delete (an editor
// showing it, a virus scanner mid-scan, another instance reading it) — and when
// the destination is a directory rather than a file.
//
// The old file is moved aside, the new one is put in place, and the old copy is
// dropped only once the new file exists. If the new file cannot be put in
// place, the old one is moved back. That ordering is the point: deleting the
// destination first, which is what this used to do, put the config in a window
// where it existed nowhere at all — and with an empty directory as the
// destination it did not even report a failure, it reported success and left a
// config file where the directory had been.
//
// A directory is refused up front rather than moved aside: `rename` means
// something other than "replace a config file" there, and relocating whatever
// it held would be a silent, larger change than the one that was asked for.
//
// `temp` is always consumed, whether this succeeds or fails.
std::expected<void, std::string> replaceFileWithBackup(std::filesystem::path temp,
                                                       const std::filesystem::path &path,
                                                       const std::filesystem::path &destination) {
    const auto discard = [&temp] {
        std::error_code cleanup;
        std::filesystem::remove(temp, cleanup);
    };

    std::error_code type_error;
    if (std::filesystem::is_directory(destination, type_error) || type_error) {
        discard();
        return std::unexpected(
            std::format("cannot write config {}: it is a directory, not a file", pathToUtf8(path)));
    }

    auto backup = destination;
    backup += std::format(".old-{}", hexId(4));

    std::error_code ec;
    std::filesystem::rename(destination, backup, ec);
    if (ec) {
        // Nothing moved, so the old file is still exactly where the caller left
        // it; only the new one has to be cleaned up.
        discard();
        return std::unexpected(
            std::format("cannot replace {}: {}", pathToUtf8(path), ec.message()));
    }

    std::error_code put_error;
    std::filesystem::rename(temp, destination, put_error);
    if (put_error) {
        std::error_code restore_error;
        std::filesystem::rename(backup, destination, restore_error);
        discard();
        // If the original could not be put back it is sitting next to the
        // destination, so say where: otherwise the message describes a lost
        // config as if nothing had happened.
        if (restore_error) {
            return std::unexpected(
                std::format("cannot replace {}: {} (the previous file is at {})", pathToUtf8(path),
                            put_error.message(), pathToUtf8(backup)));
        }
        return std::unexpected(
            std::format("cannot replace {}: {}", pathToUtf8(path), put_error.message()));
    }

    std::error_code cleanup;
    std::filesystem::remove(backup, cleanup);
    return {};
}

std::string ConfigStore::toJson() const {
    return toJsonString(config_);
}

std::expected<void, std::string> ConfigStore::save() const {
    return saveAs(path_);
}

std::expected<void, std::string> ConfigStore::saveAs(const std::filesystem::path &path) const {
    std::error_code ec;
    auto destination = path;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return std::unexpected(
            std::format("cannot inspect configuration path {}: {}", pathToUtf8(path),
                        ec.message()));
    }
    if (std::filesystem::is_symlink(status)) {
        // Keep the configured link (and therefore the path the operator wrote
        // down, and anything keyed on it) intact. Rename into the target
        // directory, never over the link itself.
        destination = std::filesystem::canonical(path, ec);
        if (ec) {
            return std::unexpected(
                std::format("cannot resolve configuration symlink {}: {}", pathToUtf8(path),
                            ec.message()));
        }
        if (!std::filesystem::is_regular_file(destination, ec)) {
            return std::unexpected(
                std::format("cannot save configuration symlink {}: {}", pathToUtf8(path),
                            ec ? ec.message() : "target is not a regular file"));
        }
    }
    ec.clear();
    const auto parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(
                std::format("cannot create {}: {}", pathToUtf8(parent), ec.message()));
        }
    }

    const std::string text = toJson() + "\n";

    // Temp file in the same directory, then rename: rename(2) within one
    // filesystem is atomic, so a reader sees either the old file or the new
    // one — never a truncated one. A temp elsewhere would fail the moment the
    // two paths crossed a mount point.
    auto temp = destination;
    temp += std::format(".tmp-{}", hexId(4));

    {
        std::ofstream output{temp, std::ios::binary | std::ios::trunc};
        if (!output) {
            return std::unexpected(std::format("cannot write {}", pathToUtf8(temp)));
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temp, ec);
            return std::unexpected(std::format("write to {} failed", pathToUtf8(temp)));
        }
    }

    std::filesystem::rename(temp, destination, ec);
    if (ec) {
        // The atomic replace was refused. Fall back to one that keeps the old
        // file until the new one is in place; its message is the one that
        // describes the state the file was left in, so it is what gets
        // reported.
        if (auto replaced = replaceFileWithBackup(temp, path, destination); !replaced) {
            return std::unexpected(replaced.error());
        }
    }

    // Keys are in here, so the file should not be readable by anyone else.
#ifndef _WIN32
    std::filesystem::permissions(destination,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#else
    restrictToOwner(destination);
#endif
    return {};
}

// ── validation ───────────────────────────────────────────────────────────────

namespace {

void addIssue(ValidationReport &report, ValidationIssue::Level level, std::string path,
              std::string message) {
    report.issues.push_back(
        ValidationIssue{.level = level, .path = std::move(path), .message = std::move(message)});
}

bool looksLikeUrl(const std::string &text) {
    return startsWith(text, "http://") || startsWith(text, "https://");
}

} // namespace

std::string ValidationReport::summary() const {
    const auto errors = count(ValidationIssue::Level::Error);
    const auto warnings = count(ValidationIssue::Level::Warning);
    if (errors == 0 && warnings == 0) {
        return "no issues";
    }
    std::string out;
    if (errors > 0) {
        out += std::format("{} error{}", errors, errors == 1 ? "" : "s");
    }
    if (warnings > 0) {
        if (!out.empty()) {
            out += ", ";
        }
        out += std::format("{} warning{}", warnings, warnings == 1 ? "" : "s");
    }
    return out;
}

std::size_t ValidationReport::count(ValidationIssue::Level level) const {
    return static_cast<std::size_t>(std::ranges::count_if(
        issues, [level](const ValidationIssue &issue) { return issue.level == level; }));
}

bool ValidationReport::ok() const {
    return count(ValidationIssue::Level::Error) == 0;
}

ValidationReport validate(const AppConfig &config) {
    ValidationReport report;

    // ── server ───────────────────────────────────────────────────────────────
    if (config.server.port < 0 || config.server.port > 65535) {
        addIssue(report, ValidationIssue::Level::Error, "server.port",
                 std::format("port {} is outside 0..65535", config.server.port));
    } else if (config.server.port == 0) {
        addIssue(report, ValidationIssue::Level::Info, "server.port",
                 "port 0 asks the kernel for a free port; the console shows the one it got");
    }
    if (config.server.host.empty()) {
        addIssue(report, ValidationIssue::Level::Error, "server.host", "host must not be empty");
    }
    const auto &tls = config.server;
    if (tls.tls_cert_file.empty() != tls.tls_key_file.empty()) {
        addIssue(report, ValidationIssue::Level::Error, "server.tls_cert_file",
                 "tls_cert_file and tls_key_file must both be set, or both empty for HTTP");
    } else if (!tls.tls_cert_file.empty()) {
        bool files_ok = true;
        for (const auto &entry : {std::pair{"server.tls_cert_file", tls.tls_cert_file},
                                  std::pair{"server.tls_key_file", tls.tls_key_file}}) {
            std::error_code ec;
            if (!std::filesystem::path{entry.second}.is_absolute() ||
                !std::filesystem::is_regular_file(entry.second, ec)) {
                addIssue(report, ValidationIssue::Level::Error, entry.first,
                         "TLS file must be an existing regular file with an absolute path");
                files_ok = false;
            }
        }
        if (files_ok) {
            const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx{
                SSL_CTX_new(TLS_server_method()), SSL_CTX_free};
            // Unattended startup must never wait for a private-key password.
            if (ctx) SSL_CTX_set_default_passwd_cb(ctx.get(),
                [](char *, int, int, void *) -> int { return 0; });
            if (!ctx || SSL_CTX_use_certificate_chain_file(ctx.get(), tls.tls_cert_file.c_str()) != 1) {
                addIssue(report, ValidationIssue::Level::Error, "server.tls_cert_file",
                         "cannot load the PEM certificate chain");
            } else {
                const X509 *cert = SSL_CTX_get0_certificate(ctx.get());
                if (X509_cmp_current_time(X509_get0_notBefore(cert)) >= 0 ||
                    X509_cmp_current_time(X509_get0_notAfter(cert)) <= 0) {
                    addIssue(report, ValidationIssue::Level::Error, "server.tls_cert_file",
                             "TLS certificate is expired, not yet valid, or has invalid dates");
                }
                if (SSL_CTX_use_PrivateKey_file(ctx.get(), tls.tls_key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
                    SSL_CTX_check_private_key(ctx.get()) != 1) {
                    addIssue(report, ValidationIssue::Level::Error, "server.tls_key_file",
                             "cannot load an unencrypted PEM private key matching the certificate");
                }
            }
        }
    }
    if (config.server.session_affinity_sec < 0) {
        addIssue(report, ValidationIssue::Level::Error, "server.session_affinity_sec",
                 "a negative affinity window would never expire; use 0 to disable it");
    }
    if (config.server.request_deadline_sec < 0) {
        addIssue(report, ValidationIssue::Level::Error, "server.request_deadline_sec",
                 "a negative deadline is not a deadline; use 0 to disable it");
    } else if (config.server.request_deadline_sec > 0 &&
               config.server.request_deadline_sec < 5) {
        addIssue(report, ValidationIssue::Level::Warning, "server.request_deadline_sec",
                 std::format("a {}s deadline leaves no room for a second relay; every request "
                             "will either succeed on the first candidate or time out",
                             config.server.request_deadline_sec));
    }
    if (config.server.routing_policy != "priority" && config.server.routing_policy != "fastest" &&
        config.server.routing_policy != "cheapest") {
        addIssue(report, ValidationIssue::Level::Warning, "server.routing_policy",
                 std::format("`{}` is not a routing policy; falling back to `priority` "
                             "(known: priority, fastest, cheapest)",
                             config.server.routing_policy));
    }
    if (config.server.circuit_failure_threshold < 1) {
        addIssue(report, ValidationIssue::Level::Warning, "server.circuit_failure_threshold",
                 "values below 1 disable circuit breaking entirely");
    }
    if (config.server.circuit_cooldown_sec < 1) {
        addIssue(report, ValidationIssue::Level::Warning, "server.circuit_cooldown_sec",
                 "values below 1 leave no cooldown window");
    }
    if (config.server.log_capacity < 16) {
        addIssue(report, ValidationIssue::Level::Warning, "server.log_capacity",
                 "a log smaller than 16 entries makes the live view useless");
    }
    if (config.server.persist_telemetry && config.server.log_bodies) {
        // Not an error: the operator asked for both. But bodies are prompts, and
        // this combination is the one that puts them on disk.
        addIssue(report, ValidationIssue::Level::Warning, "server.persist_telemetry",
                 "persist_telemetry with log_bodies on writes request and response bodies "
                 "to the telemetry file, where they outlive the process");
    }
    if (config.server.api_key.empty() && config.server.host != "127.0.0.1" &&
        config.server.host != "localhost" && config.server.host != "::1") {
        addIssue(report, ValidationIssue::Level::Warning, "server.api_key",
                 std::format("`{}` is reachable off this machine but no server.api_key is set",
                             config.server.host));
    }
    if (config.server.web_ui && config.server.api_key.empty() &&
        config.server.host != "127.0.0.1" && config.server.host != "localhost" &&
        config.server.host != "::1") {
        // Not an error: the operator may be behind a firewall they control. But
        // the console can read the request log, so reaching it must be a choice.
        addIssue(report, ValidationIssue::Level::Warning, "server.web_ui",
                 std::format("the web console is served on `{}` with no server.api_key; "
                             "anyone who can reach the port can read the request log",
                             config.server.host));
    }
    if (config.server.traffic_bucket_sec < 60) {
        addIssue(report, ValidationIssue::Level::Warning, "server.traffic_bucket_sec",
                 std::format("a {}s bucket is finer than the trend chart can label; "
                             "raised to 60",
                             config.server.traffic_bucket_sec));
    }
    if (config.server.traffic_bucket_count < 2) {
        addIssue(report, ValidationIssue::Level::Warning, "server.traffic_bucket_count",
                 "a trend needs at least two buckets to show a shape");
    } else if (config.server.traffic_bucket_count > 10000) {
        addIssue(report, ValidationIssue::Level::Warning, "server.traffic_bucket_count",
                 std::format("{} buckets is a very large trend to persist and render",
                             config.server.traffic_bucket_count));
    }
    if (config.server.response_cache_ttl_sec < 0) {
        addIssue(report, ValidationIssue::Level::Error, "server.response_cache_ttl_sec",
                 "a negative TTL would expire entries before they are stored; use 0 to disable the cache");
    }
    if (config.server.response_cache_max_entries < 0) {
        addIssue(report, ValidationIssue::Level::Error, "server.response_cache_max_entries",
                 "the entry count cannot be negative");
    } else if (config.server.response_cache_ttl_sec > 0 &&
               config.server.response_cache_max_entries < 1) {
        addIssue(report, ValidationIssue::Level::Error, "server.response_cache_max_entries",
                 "a cache with no entries cannot store anything; raise it or set "
                 "response_cache_ttl_sec to 0");
    }
    if (!config.server.otlp_endpoint.empty() &&
        !looksLikeUrl(config.server.otlp_endpoint)) {
        addIssue(report, ValidationIssue::Level::Error, "server.otlp_endpoint",
                 "otlp_endpoint must start with http:// or https://");
    }

    // ── providers ────────────────────────────────────────────────────────────
    std::set<std::string, std::less<>> seenIds;
    for (std::size_t i = 0; i < config.providers.size(); ++i) {
        const auto &entry = config.providers[i];
        const std::string where = std::format("providers[{}]", i);

        if (entry.id.empty()) {
            addIssue(report, ValidationIssue::Level::Error, where + ".id",
                     "id must not be empty — routes and logs refer to a relay by id");
        } else if (!seenIds.insert(entry.id).second) {
            addIssue(report, ValidationIssue::Level::Error, where + ".id",
                     std::format("duplicate id `{}`", entry.id));
        }
        const std::string label = entry.id.empty() ? where : entry.id;

        if (entry.price_in_per_million < 0.0 || entry.price_out_per_million < 0.0) {
            addIssue(report, ValidationIssue::Level::Error, where + ".price_in_per_million",
                     "prices are dollars per million tokens and cannot be negative");
        }
        for (const auto &[model, pricing] : entry.model_prices) {
            if (pricing.price_in_per_million < 0.0 || pricing.price_out_per_million < 0.0) {
                addIssue(report, ValidationIssue::Level::Error,
                         std::format("{}.model_prices[\"{}\"]", where, model),
                         "prices are dollars per million tokens and cannot be negative");
            }
        }
        if (entry.base_url.empty()) {
            addIssue(report, ValidationIssue::Level::Error, where + ".base_url",
                     std::format("relay `{}` has no base_url", label));
        } else if (!looksLikeUrl(entry.base_url)) {
            addIssue(report, ValidationIssue::Level::Error, where + ".base_url",
                     "base_url must start with http:// or https://");
        }

        if (isSecretReference(entry.api_key)) {
            const std::string name = secretReferenceName(entry.api_key);
            if (name.empty()) {
                addIssue(report, ValidationIssue::Level::Error, where + ".api_key",
                         "malformed environment reference");
            } else if (std::getenv(name.c_str()) == nullptr) {
                // Info, not Error: a relay with an unset key is still a valid
                // disabled-ish entry, and the console surfaces it as a row
                // that will fail rather than as a config that will not load.
                addIssue(report, ValidationIssue::Level::Warning, where + ".api_key",
                         std::format("`{}` is not set in this shell", name));
            }
        }

        if (entry.timeout_sec < 1) {
            addIssue(report, ValidationIssue::Level::Warning, where + ".timeout_sec",
                     "a timeout below 1s will fail most real completions");
        }
        if (entry.max_concurrent < 0 || entry.requests_per_minute < 0) {
            addIssue(report, ValidationIssue::Level::Error, where,
                     "relay concurrency and rate limits cannot be negative");
        }
        if (entry.priority < 0) {
            addIssue(report, ValidationIssue::Level::Warning, where + ".priority",
                     "negative priorities sort before every default entry");
        }
        if (!entry.protocol.empty()) {
            const std::string proto = toLower(entry.protocol);
            if (proto != "openai" && proto != "openai_compatible" && proto != "openai_chat" &&
                proto != "anthropic" && proto != "gemini" && proto != "openai_responses" &&
                proto != "azure" && proto != "vertex" && proto != "bedrock" &&
                proto != "ollama") {
                addIssue(report, ValidationIssue::Level::Warning, where + ".protocol",
                         std::format("unrecognised protocol `{}`; supported: openai, anthropic, "
                                     "gemini, openai_responses, azure, vertex, bedrock, ollama",
                                     entry.protocol));
            } else if (proto == "vertex") {
                // The path is /v1/projects/{project}/locations/{region}/...; a
                // missing project cannot be guessed from the key file, and a
                // wrong one is a 404 the operator would blame on the relay.
                if (entry.project.empty()) {
                    addIssue(report, ValidationIssue::Level::Error, where + ".project",
                             "a Vertex relay needs `project` (and `credentials_file`)");
                }
                if (entry.credentials_file.empty()) {
                    addIssue(report, ValidationIssue::Level::Error, where + ".credentials_file",
                             "a Vertex relay needs a service-account JSON key to exchange for "
                             "an access token");
                } else {
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(entry.credentials_file, ec)) {
                        addIssue(report, ValidationIssue::Level::Error,
                                 where + ".credentials_file",
                                 "credentials_file is not a readable regular file");
                    }
                }
            } else if (proto == "bedrock") {
                if (entry.region.empty()) {
                    addIssue(report, ValidationIssue::Level::Error, where + ".region",
                             "Bedrock signs every request against a region and cannot guess one");
                }
                if (entry.aws_access_key.empty() || entry.aws_secret_key.empty()) {
                    addIssue(report, ValidationIssue::Level::Error, where,
                             "a Bedrock relay needs aws_access_key and aws_secret_key to sign "
                             "its requests (SigV4); `${ENV_VAR}` references are resolved at "
                             "signing time");
                }
            }
        }
        if (entry.enabled && entry.models.empty()) {
            addIssue(report, ValidationIssue::Level::Info, where + ".models",
                     std::format("relay `{}` lists no models; it is only reachable through a "
                                 "route that names it explicitly",
                                 label));
        }
    }

    // ── routes ───────────────────────────────────────────────────────────────
    std::set<std::string, std::less<>> seenModels;
    for (std::size_t i = 0; i < config.routes.size(); ++i) {
        const auto &route = config.routes[i];
        const std::string where = std::format("routes[{}]", i);

        if (route.model.empty()) {
            addIssue(report, ValidationIssue::Level::Error, where + ".model",
                     "a route needs a model name to match on");
        } else if (!seenModels.insert(route.model).second) {
            addIssue(report, ValidationIssue::Level::Error, where + ".model",
                     std::format("two routes both claim `{}`; only the first is used",
                                 route.model));
        }
        if (route.targets.empty()) {
            addIssue(report, ValidationIssue::Level::Error, where + ".targets",
                     std::format("route `{}` has no targets", route.model));
        }
        for (std::size_t t = 0; t < route.targets.size(); ++t) {
            const auto &target = route.targets[t];
            const std::string twhere = std::format("{}.targets[{}]", where, t);
            if (target.provider.empty()) {
                addIssue(report, ValidationIssue::Level::Error, twhere + ".provider",
                         "target has no provider id");
                continue;
            }
            const auto *provider = config.provider(target.provider);
            if (provider == nullptr) {
                addIssue(report, ValidationIssue::Level::Error, twhere + ".provider",
                         std::format("no relay with id `{}`", target.provider));
            } else if (!provider->enabled) {
                addIssue(report, ValidationIssue::Level::Warning, twhere + ".provider",
                         std::format("relay `{}` is disabled, so this hop is skipped",
                                     target.provider));
            }
        }
    }

    if (config.providers.empty()) {
        addIssue(report, ValidationIssue::Level::Warning, "providers",
                 "no relays configured — every chat request will fail with 503");
    } else if (std::ranges::none_of(config.providers,
                                    [](const ProviderConfig &p) { return p.enabled; })) {
        addIssue(report, ValidationIssue::Level::Warning, "providers",
                 "every relay is disabled — every chat request will fail with 503");
    }

    return report;
}

} // namespace literouter
