// Per-relay admission. This answers one of the two "may this request go out?"
// questions, and it is a different question from the other:
//
//   Router          — is this relay *failing*? (breaker state)
//   UpstreamLimiter — is this relay *full*? (what we are already sending it)
//
// The remedies differ, which is why they are not one thing: a failing relay gets
// skipped until its cooldown, while a full relay gets skipped for *this* request
// and the next candidate answers. Conflating "full" with "failing" would trip a
// breaker on a relay that is serving perfectly and just happens to be busy.
module;

module literouter.core;

import std;

namespace literouter {

namespace {

int retrySeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return 1;
    }
    return static_cast<int>(std::clamp(std::ceil(seconds), 1.0,
                                       static_cast<double>(std::numeric_limits<int>::max())));
}

// A hard ceiling on the rolling window, so a relay with a rate limit of a
// million requests per minute cannot turn the limiter itself into the memory
// problem. One minute of starts is all the window needs; past this many the
// oldest are dropped, which makes the limit behave as "unlimited" rather than
// as "reject everything" — the safe direction for a bound nobody should reach.
constexpr std::size_t kMaxWindowEntries = 100'000;

} // namespace

// ── UpstreamSlot ─────────────────────────────────────────────────────────────

struct UpstreamSlot::Impl {
    // A std::function rather than a back-pointer to the limiter's Impl: the
    // slot is destroyed from whatever thread finished the response, possibly
    // after the limiter's config was replaced, and the callback holds a weak
    // reference so a slot outliving the limiter releases nothing rather than
    // touching freed memory.
    std::function<void()> release;
    ~Impl() {
        if (release) {
            release();
        }
    }
};

UpstreamSlot::UpstreamSlot() : impl_(std::make_shared<Impl>()) {}
UpstreamSlot::~UpstreamSlot() = default;

// ── UpstreamLimiter ──────────────────────────────────────────────────────────

struct UpstreamLimiter::Impl {
    struct State {
        int max_concurrent = 0;
        int requests_per_minute = 0;
        int active = 0;
        // True once the relay has left the config while still holding a slot.
        // The entry cannot simply be erased: the slot that is still out will
        // come back and decrement a counter, and if the relay had been re-added
        // in the meantime that counter belongs to a different generation —
        // which would let the new one be over-admitted. Keeping the entry until
        // its last slot returns is what makes generations impossible to mix.
        bool retired = false;
        std::deque<double> recent;
    };
    mutable std::mutex mutex;
    std::map<std::string, State, std::less<>> states;
};

UpstreamLimiter::UpstreamLimiter() : impl_(std::make_shared<Impl>()) {}
UpstreamLimiter::~UpstreamLimiter() = default;

void UpstreamLimiter::setConfig(const AppConfig &config) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    // Limits are refreshed in place rather than rebuilt: a relay answering right
    // now holds `active` and a window of starts that belong to it, and dropping
    // them would let a config edit hand out slots that are already taken.
    for (const auto &provider : config.providers) {
        auto &state = impl->states[provider.id];
        state.max_concurrent = std::max(0, provider.max_concurrent);
        state.requests_per_minute = std::max(0, provider.requests_per_minute);
        state.retired = false;
    }
    for (auto &[id, state] : impl->states) {
        if (config.provider(id) != nullptr) {
            continue;
        }
        // A relay the config no longer names stops admitting immediately, but
        // its counters stay until the slots it handed out are released.
        state.retired = true;
        state.max_concurrent = 0;
        state.requests_per_minute = 0;
        state.recent.clear();
    }
}

std::expected<std::shared_ptr<UpstreamSlot>, UpstreamRejection> UpstreamLimiter::admit(
    std::string_view provider, double now_unix) {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    auto &state = impl->states[std::string{provider}];
    if (state.retired) {
        return std::unexpected(UpstreamRejection{
            std::format("relay `{}` is no longer in the configuration", provider), 1});
    }
    while (!state.recent.empty() && state.recent.front() <= now_unix - 60.0) {
        state.recent.pop_front();
    }
    if (state.max_concurrent > 0 && state.active >= state.max_concurrent) {
        return std::unexpected(UpstreamRejection{
            std::format("relay `{}` already has {} request(s) in flight", provider, state.active),
            1});
    }
    if (state.requests_per_minute > 0 &&
        state.recent.size() >= static_cast<std::size_t>(state.requests_per_minute)) {
        return std::unexpected(UpstreamRejection{
            std::format("relay `{}` has taken {} request(s) in the last minute", provider,
                        state.recent.size()),
            retrySeconds(state.recent.front() + 60.0 - now_unix)});
    }
    ++state.active;
    // Every start is recorded, whether or not a rate limit is in force. Keeping
    // the window only while a limit is set would mean an operator who turns a
    // limit on gets a free first minute — the requests that just happened would
    // not be counted against it — which is precisely when the limit was wanted.
    // The window is bounded, so the memory cost of counting is a fixed ceiling
    // per relay rather than a function of traffic.
    if (state.recent.size() < kMaxWindowEntries) {
        state.recent.push_back(now_unix);
    }

    std::weak_ptr<Impl> weak = impl;
    const std::string id{provider};
    auto slot = std::make_shared<UpstreamSlot>();
    slot->impl_->release = [weak, id] {
        if (auto owner = weak.lock()) {
            std::scoped_lock guard{owner->mutex};
            const auto it = owner->states.find(id);
            if (it == owner->states.end()) {
                return;
            }
            if (it->second.active > 0) {
                --it->second.active;
            }
            // The last slot of a relay that has left the config is what lets its
            // entry go, so a re-added relay starts with no inherited state.
            if (it->second.active == 0 && it->second.retired) {
                owner->states.erase(it);
            }
        }
    };
    return slot;
}

std::size_t UpstreamLimiter::active(std::string_view provider) const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    const auto it = impl->states.find(provider);
    return it == impl->states.end() ? 0 : static_cast<std::size_t>(it->second.active);
}

int UpstreamLimiter::limitFor(std::string_view provider) const {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    const auto it = impl->states.find(provider);
    return it == impl->states.end() ? 0 : it->second.max_concurrent;
}

void UpstreamLimiter::reset() {
    const auto impl = impl_;
    std::scoped_lock lock{impl->mutex};
    for (auto &[id, state] : impl->states) {
        state.active = 0;
        state.recent.clear();
    }
}

} // namespace literouter
