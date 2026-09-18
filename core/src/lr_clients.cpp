// Client access and quota accounting. State is separate from optional telemetry:
// turning off logs or resetting charts must never grant another day's quota.
module;
#include <errno.h>
#include <stdio.h>
#include <openssl/rand.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

module literouter.core;
import std;
import nlohmann.json;

namespace literouter {
namespace {
using json = nlohmann::json;

std::uint64_t plus(std::uint64_t a, std::uint64_t b) {
    return b > std::numeric_limits<std::uint64_t>::max() - a
               ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

int retrySeconds(double seconds) {
    return static_cast<int>(std::clamp(std::ceil(seconds), 1.0,
                                      static_cast<double>(std::numeric_limits<int>::max())));
}

json usageJson(const ClientUsage &u) {
    return json{{"client", u.client}, {"requests", u.requests}, {"successes", u.successes},
                {"failures", u.failures}, {"tokens_prompt", u.tokens_prompt},
                {"tokens_completion", u.tokens_completion}, {"cost_usd", u.cost_usd},
                {"active_requests", u.active_requests}, {"day_unix", u.day_unix},
                {"requests_today", u.requests_today}, {"tokens_today", u.tokens_today},
                {"reserved_tokens", u.reserved_tokens}};
}

std::uint64_t counter(const json &j, const char *name) {
    const auto &value = j.at(name);
    if (!value.is_number_unsigned()) throw std::runtime_error("invalid quota counter");
    return value.get<std::uint64_t>();
}

// Lock a separate stable inode because saving the JSON atomically replaces its
// inode. Never unlink the lock file: an old and a new inode could then both be
// owned. The OS releases ownership if the process exits unexpectedly.
class QuotaStateLock {
public:
    ~QuotaStateLock() { release(); }
    bool held() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }
    std::expected<void, std::string> acquire(const std::filesystem::path &state_path) {
        if (held()) return {};
        std::error_code ec;
        auto parent = state_path.parent_path();
        if (parent.empty()) parent = ".";
        std::filesystem::create_directories(parent, ec);
        if (ec) return std::unexpected("cannot create client quota state directory");
        auto lock_path = state_path;
        lock_path += ".lock";
#ifdef _WIN32
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return std::unexpected("client quota state is already in use or cannot be locked");
#else
        fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0)
            return std::unexpected("cannot open client quota state lock");
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            release();
            return std::unexpected("client quota state is already in use or cannot be locked");
        }
#endif
        return {};
    }
    void release() {
#ifdef _WIN32
        if (held()) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (held()) { ::close(fd_); fd_ = -1; }
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

std::expected<void, std::string> saveState(const std::filesystem::path &path,
                                           const std::string &text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected("cannot create client quota state directory");
    auto temp = path;
    temp += ".tmp-" + hexId(8);
#ifdef _WIN32
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::unexpected("cannot write client quota state");
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    const bool replaced = ok && MoveFileExW(temp.c_str(), path.c_str(),
                                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return std::unexpected("cannot write client quota state");
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
    const bool ok = offset == text.size() && ::fsync(fd) == 0;
    ::close(fd);
    bool replaced = ok && ::rename(temp.c_str(), path.c_str()) == 0;
    if (replaced) {
        const int dir = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir >= 0) { replaced = ::fsync(dir) == 0; ::close(dir); }
        else replaced = false;
    }
#endif
    if (!replaced) {
        std::filesystem::remove(temp, ec);
        return std::unexpected("cannot persist client quota state; requests are blocked");
    }
    return {};
}
} // namespace

std::optional<ClientIdentity> authenticateClient(const AppConfig &config,
                                                 std::string_view presented, bool management) {
    std::optional<ClientIdentity> found;
    const std::string admin = resolveSecret(config.server.api_key);
    if (!admin.empty() && !presented.empty() && secureEquals(admin, presented))
        found = ClientIdentity{.administrator = true};
    // Check every key: duplicate resolved environment references must not select
    // an arbitrary account, even when only one of the accounts is enabled.
    bool duplicate = false;
    for (const auto &client : config.clients) {
        for (const auto &key : client.keys) {
            const auto secret = resolveSecret(key.api_key);
            if (secret.empty() || presented.empty() || !secureEquals(secret, presented)) continue;
            if (found) duplicate = true;
            found = ClientIdentity{client.id, key.id, false};
            if (!client.enabled || !key.enabled) duplicate = true;
        }
    }
    if (duplicate) return std::nullopt;
    if (found && (!management || found->administrator)) return found;
    if (found) return std::nullopt;
    if (config.clients.empty() && config.server.api_key.empty())
        return ClientIdentity{.administrator = true};
    return std::nullopt;
}

std::expected<std::string, std::string> generateClientKey() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return std::unexpected("cannot obtain cryptographic randomness");
    constexpr char digits[] = "0123456789abcdef";
    std::string key = "lr_";
    for (const auto byte : bytes) {
        key += digits[byte >> 4];
        key += digits[byte & 15];
    }
    return key;
}

bool clientAllowsModel(const ClientConfig &client, std::string_view model) {
    return client.models.empty() || std::ranges::find(client.models, model) != client.models.end();
}

bool clientAllowsProvider(const ClientConfig &client, const ProviderConfig &provider) {
    return client.provider_groups.empty() || std::ranges::any_of(provider.groups, [&](const auto &group) {
        return std::ranges::find(client.provider_groups, group) != client.provider_groups.end();
    });
}

struct ClientLedger::Impl {
    struct Account {
        ClientUsage usage;
        std::deque<double> recent;
    };
    mutable std::mutex mutex;
    std::filesystem::path path;
    std::map<std::string, Account, std::less<>> accounts;
    bool healthy = false;
    bool accepting = false;
    std::size_t outstanding = 0;
    QuotaStateLock ownership;

    std::expected<void, std::string> load();
    std::expected<void, std::string> acquire() {
        if (ownership.held()) return {};
        if (auto locked = ownership.acquire(path); !locked) return locked;
        auto loaded = load(); // A lazy opener must re-read under the lock.
        if (!loaded) ownership.release();
        return loaded;
    }

    static void roll(Account &a, double now) {
        const double day = std::floor(now / 86400.0) * 86400.0;
        if (day > a.usage.day_unix) {
            a.usage.day_unix = day;
            a.usage.requests_today = 0;
            a.usage.tokens_today = 0;
            a.usage.reserved_tokens = 0;
        }
        while (!a.recent.empty() && a.recent.front() <= now - 60.0) a.recent.pop_front();
    }

    bool save() {
        json entries = json::array();
        for (const auto &[id, a] : accounts) {
            auto entry = usageJson(a.usage);
            entry["recent"] = a.recent;
            entries.push_back(std::move(entry));
        }
        healthy = saveState(path, json{{"schema", 1}, {"clients", entries}}.dump()).has_value();
        return healthy;
    }
};

ClientRequest::ClientRequest(std::function<void(bool, std::uint64_t, std::uint64_t, double, bool)> done)
    : done_(std::move(done)) {}

ClientRequest::~ClientRequest() {
    try { finish(false, 0, 0, 0.0, false); } catch (...) { /* quota remains reserved */ }
}

void ClientRequest::finish(bool success, std::uint64_t prompt, std::uint64_t completion,
                           double cost, bool known) {
    if (!finished_.exchange(true)) done_(success, prompt, completion, cost, known);
}

ClientLedger::ClientLedger() : impl_(std::make_shared<Impl>()) {}
ClientLedger::~ClientLedger() { close(); }

std::expected<void, std::string> ClientLedger::open(const std::filesystem::path &path,
                                                   bool require_ownership) {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->outstanding > 0)
        return std::unexpected("client quota requests are still active");
    impl_->ownership.release();
    impl_->accepting = false;
    impl_->healthy = false;
    impl_->path = path;
    impl_->accounts.clear();
    std::error_code ec;
    if (std::filesystem::exists(path.parent_path(), ec) &&
        !std::filesystem::is_directory(path.parent_path(), ec))
        return std::unexpected("client quota state parent is not a directory");
    if (ec) return std::unexpected("cannot inspect client quota state directory");
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return std::unexpected("cannot inspect client quota state");
    if (exists || require_ownership) {
        if (auto acquired = impl_->acquire(); !acquired) return acquired;
    } else {
        impl_->healthy = true;
    }
    impl_->accepting = true;
    return {};
}

void ClientLedger::close() {
    std::scoped_lock lock{impl_->mutex};
    impl_->accepting = false;
    if (impl_->outstanding == 0) impl_->ownership.release();
}

std::expected<void, std::string> ClientLedger::Impl::load() {
    healthy = false;
    accounts.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return std::unexpected("cannot inspect client quota state");
    if (!exists) { healthy = true; return {}; }
    try {
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::runtime_error("cannot read quota file");
        json root = json::parse(in);
        if (root.at("schema") != 1 || !root.at("clients").is_array())
            throw std::runtime_error("invalid quota schema");
        for (const auto &entry : root.at("clients")) {
            Impl::Account a;
            auto &u = a.usage;
            u.client = entry.at("client").get<std::string>();
            u.requests = counter(entry, "requests");
            u.successes = counter(entry, "successes");
            const auto active = counter(entry, "active_requests");
            const auto failures = counter(entry, "failures");
            u.failures = plus(failures, active);
            u.tokens_prompt = counter(entry, "tokens_prompt");
            u.tokens_completion = counter(entry, "tokens_completion");
            u.cost_usd = entry.at("cost_usd").get<double>();
            u.day_unix = entry.at("day_unix").get<double>();
            u.requests_today = counter(entry, "requests_today");
            u.tokens_today = counter(entry, "tokens_today");
            const auto reserved = counter(entry, "reserved_tokens");
            if (!entry.at("recent").is_array() || reserved > u.tokens_today ||
                u.requests_today > u.requests || plus(u.successes, u.failures) != u.requests ||
                entry.at("recent").size() > u.requests)
                throw std::runtime_error("inconsistent quota counters");
            constexpr double max_timestamp = 253402300799.0; // last second of year 9999
            if (u.client.empty() || !std::isfinite(u.cost_usd) || u.cost_usd < 0 ||
                !std::isfinite(u.day_unix) || u.day_unix < 0 || u.day_unix > max_timestamp ||
                std::fmod(u.day_unix, 86400.0) != 0 || accounts.contains(u.client))
                throw std::runtime_error("invalid quota account");
            for (const auto &value : entry.at("recent")) {
                const double time = value.get<double>();
                if (!std::isfinite(time) || time < 0 || time > max_timestamp ||
                    (!a.recent.empty() && time < a.recent.back()))
                    throw std::runtime_error("invalid quota request window");
                a.recent.push_back(time);
            }
            Impl::roll(a, nowUnix());
            const auto id = u.client;
            accounts.emplace(id, std::move(a));
        }
        healthy = true;
    } catch (...) {
        accounts.clear();
        return std::unexpected("client quota state is unreadable or invalid; restore it before serving clients");
    }
    return {};
}

std::expected<std::shared_ptr<ClientRequest>, ClientRejection> ClientLedger::admit(
    const ClientConfig &client, std::uint64_t estimate) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    if (!impl->accepting || !impl->healthy)
        return std::unexpected(ClientRejection{503, "client quota storage unavailable", 1});
    if (auto acquired = impl->acquire(); !acquired)
        return std::unexpected(ClientRejection{503, acquired.error(), 1});
    auto &a = impl->accounts[client.id];
    auto &u = a.usage;
    u.client = client.id;
    const double now = nowUnix();
    Impl::roll(a, now);
    const int tomorrow = retrySeconds(u.day_unix + 86400.0 - now);
    if (client.max_concurrent > 0 && u.active_requests >= static_cast<std::uint64_t>(client.max_concurrent))
        return std::unexpected(ClientRejection{429, "client concurrency limit reached", 1});
    if (client.requests_per_minute > 0 && a.recent.size() >= static_cast<std::size_t>(client.requests_per_minute))
        return std::unexpected(ClientRejection{429, "client request rate exceeded",
            retrySeconds(a.recent.front() + 60.0 - now)});
    if (client.requests_per_day > 0 && u.requests_today >= client.requests_per_day)
        return std::unexpected(ClientRejection{429, "client daily request quota exhausted", tomorrow});
    const auto reserve = client.tokens_per_day > 0 ? std::max(client.token_reservation, estimate) : 0;
    if (client.tokens_per_day > 0 && (u.tokens_today >= client.tokens_per_day ||
        reserve > client.tokens_per_day - u.tokens_today))
        return std::unexpected(ClientRejection{429, "client daily token quota cannot cover this request", tomorrow});
    const auto before = a;
    u.requests = plus(u.requests, 1);
    u.requests_today = plus(u.requests_today, 1);
    u.active_requests = plus(u.active_requests, 1);
    u.tokens_today = plus(u.tokens_today, reserve);
    u.reserved_tokens = plus(u.reserved_tokens, reserve);
    // Wall clocks may move backwards; keeping the persisted window sorted
    // prevents a normal NTP correction from invalidating quota state on restart.
    a.recent.insert(std::upper_bound(a.recent.begin(), a.recent.end(), now), now);
    if (!impl->save()) {
        a = before;
        return std::unexpected(ClientRejection{503, "cannot persist client quota reservation", 1});
    }
    const double admitted_day = u.day_unix;
    auto done = [impl, id = client.id, reserve, admitted_day](bool success, std::uint64_t prompt,
        std::uint64_t completion, double cost, bool known) {
        std::scoped_lock guard{impl->mutex};
        auto &a = impl->accounts.at(id);
        Impl::roll(a, nowUnix());
        auto &u = a.usage;
        if (u.active_requests > 0) --u.active_requests;
        if (success) u.successes = plus(u.successes, 1); else u.failures = plus(u.failures, 1);
        u.tokens_prompt = plus(u.tokens_prompt, prompt);
        u.tokens_completion = plus(u.tokens_completion, completion);
        if (std::isfinite(cost) && cost > 0)
            u.cost_usd += std::min(cost, std::numeric_limits<double>::max() - u.cost_usd);
        // A response crossing midnight belongs to its admission day. Its
        // concurrency remains global, while yesterday's reserve cannot subtract
        // from today's account or spend today's allowance.
        if (u.day_unix == admitted_day) {
            u.reserved_tokens -= std::min(u.reserved_tokens, reserve);
            const auto observed = plus(prompt, completion);
            if (known) {
                u.tokens_today -= std::min(u.tokens_today, reserve);
                u.tokens_today = plus(u.tokens_today, observed);
            } else if (observed > reserve) {
                // Interrupted usage is incomplete, but still a known lower
                // bound. It cannot reduce the reserve or discard usage that
                // already exceeded that reserve before the interruption.
                u.tokens_today = plus(u.tokens_today, observed - reserve);
            }
        }
        impl->save();
        --impl->outstanding;
        if (!impl->accepting && impl->outstanding == 0) impl->ownership.release();
    };
    auto request = std::make_shared<ClientRequest>(std::move(done));
    ++impl->outstanding;
    return request;
}

std::vector<ClientUsage> ClientLedger::snapshot() const {
    std::scoped_lock lock{impl_->mutex};
    std::vector<ClientUsage> out;
    for (const auto &[id, stored] : impl_->accounts) {
        auto a = stored;
        Impl::roll(a, nowUnix());
        out.push_back(std::move(a.usage));
    }
    return out;
}

std::string toJsonString(const ClientUsage &usage) { return usageJson(usage).dump(); }
} // namespace literouter
