// The local response cache. Deliberately the narrowest useful thing: an exact
// match on everything that could change the answer, a TTL, a bounded entry
// count, and no streaming.
//
// What it is for: an aggregator is a proxy in front of relays that charge per
// token, and the same question is asked again constantly — a client retrying, a
// UI re-rendering, an eval loop being re-run, the same system prompt plus the
// same user turn. Answering the second one from memory costs nothing upstream
// and returns in microseconds instead of seconds.
//
// What it deliberately is not:
//   * not semantic — a request that differs by one character is a different
//     key, because "close enough" in a cache means serving an answer to a
//     question that was not asked;
//   * not streaming — replaying a cached body as an event stream would mean
//     inventing chunk boundaries and timing, and a client that measures
//     time-to-first-token would be lied to.
module;
#include <openssl/sha.h>

module literouter.core;

import std;

namespace literouter {

struct ResponseCache::Impl {
    struct Entry {
        CachedResponse response;
        double expires_unix = 0.0;
        // Monotonic use counter rather than a timestamp: two entries stored in
        // the same millisecond must still have a defined order, and the LRU
        // decision is about the order of use, not about wall-clock time.
        std::uint64_t used = 0;
    };
    mutable std::mutex mutex;
    bool enabled = false;
    double ttl_sec = 0.0;
    std::size_t max_entries = 128;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t seq = 0;
    std::map<std::string, Entry, std::less<>> entries;
};

ResponseCache::ResponseCache() : impl_(std::make_shared<Impl>()) {}
ResponseCache::~ResponseCache() = default;

void ResponseCache::configure(int ttl_sec, int max_entries) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    impl->ttl_sec = std::max(0, ttl_sec);
    impl->max_entries = static_cast<std::size_t>(std::max(0, max_entries));
    impl->enabled = impl->ttl_sec > 0 && impl->max_entries > 0;
    if (!impl->enabled) {
        // Switched off means switched off: keeping the entries would let a
        // later re-enable serve an answer from before the operator turned it
        // off, which is not what "off" means.
        impl->entries.clear();
    }
}

bool ResponseCache::enabled() const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    return impl->enabled;
}

std::string ResponseCache::keyFor(std::string_view protocol, std::string_view model,
                                 std::string_view body) {
    // Length-prefixed fields rather than a separator join: a separator that
    // appears in a body (and `\n` certainly does, in any prompt) would make two
    // different requests hash to the same key. The version prefix is what makes
    // a change to this scheme invalidate old entries instead of misreading them.
    std::string material{"literouter-cache-v1"};
    const auto append = [&material](std::string_view field) {
        material += std::format("\n{}:", field.size());
        material.append(field);
    };
    append(protocol);
    append(model);
    append(body);

    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(),
           digest.data());
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

std::optional<CachedResponse> ResponseCache::lookup(std::string_view key, double now_unix) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    if (!impl->enabled) {
        return std::nullopt;
    }
    const auto it = impl->entries.find(key);
    if (it == impl->entries.end()) {
        ++impl->misses;
        return std::nullopt;
    }
    if (it->second.expires_unix <= now_unix) {
        impl->entries.erase(it);
        ++impl->misses;
        return std::nullopt;
    }
    it->second.used = ++impl->seq;
    ++impl->hits;
    return it->second.response;
}

void ResponseCache::store(std::string_view key, CachedResponse response, double now_unix) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    if (!impl->enabled) {
        return;
    }
    if (!impl->entries.contains(key) && impl->entries.size() >= impl->max_entries) {
        // Expired entries first — evicting a live one while a dead one holds a
        // slot would throw away a hit for nothing.
        std::erase_if(impl->entries,
                      [now_unix](const auto &entry) { return entry.second.expires_unix <= now_unix; });
    }
    while (impl->entries.size() >= impl->max_entries) {
        if (impl->entries.empty()) {
            return;
        }
        auto oldest = impl->entries.begin();
        for (auto it = impl->entries.begin(); it != impl->entries.end(); ++it) {
            if (it->second.used < oldest->second.used) {
                oldest = it;
            }
        }
        impl->entries.erase(oldest);
    }
    Impl::Entry entry;
    entry.response = std::move(response);
    entry.expires_unix = now_unix + impl->ttl_sec;
    entry.used = ++impl->seq;
    impl->entries[std::string{key}] = std::move(entry);
}

std::size_t ResponseCache::size() const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    return impl->entries.size();
}

std::uint64_t ResponseCache::hits() const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    return impl->hits;
}

std::uint64_t ResponseCache::misses() const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    return impl->misses;
}

void ResponseCache::clear() {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    impl->entries.clear();
    impl->hits = 0;
    impl->misses = 0;
}

} // namespace literouter
