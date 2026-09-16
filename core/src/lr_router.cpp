// ROUTE constant_order:
//   1. an explicit route's target list, in the order the user wrote it —
//      failover chains are authored, not computed;
//   2. otherwise every enabled relay that advertises the model, ordered by
//      priority then weight then declaration.
//
// On top of that order sits the only dynamic input: a relay whose breaker is
// open is marked `skipped`. It stays in the list on purpose — when every
// candidate is skipped the proxy still has to try one, and "the least recently
// failed" is a better guess than "give up".
module literouter.core;

import std;

namespace literouter {

Router::Router() = default;

const ProviderConfig *Router::find(std::string_view id) const {
    return config_.provider(id);
}

ProviderHealth &Router::slot(std::string_view id) {
    if (auto it = health_.find(id); it != health_.end()) {
        return it->second;
    }
    ProviderHealth fresh;
    fresh.provider = std::string{id};
    return health_.emplace(std::string{id}, std::move(fresh)).first->second;
}

void Router::setConfig(const AppConfig &config) {
    std::scoped_lock lock{mutex_};
    config_ = config;
}

const AppConfig &Router::config() const {
    return config_;
}

std::vector<Candidate> Router::candidatesFor(std::string_view model) const {
    std::scoped_lock lock{mutex_};
    const double now = nowUnix();
    std::vector<Candidate> out;

    const auto push = [&](const ProviderConfig &provider, std::string upstream_model,
                          std::size_t ordinal) {
        Candidate candidate;
        candidate.provider = provider.id;
        candidate.model = upstream_model.empty() ? std::string{model} : std::move(upstream_model);
        candidate.priority = provider.priority * 1000 + static_cast<int>(ordinal);
        candidate.skipped = circuitOpen(provider.id, now);
        out.push_back(std::move(candidate));
    };

    if (const auto *route = config_.route(model); route != nullptr && route->enabled) {
        for (const auto &target : route->targets) {
            const auto *provider = config_.provider(target.provider);
            if (provider == nullptr || !provider->enabled) {
                continue;
            }
            push(*provider, target.model, out.size());
        }
    }

    if (out.empty() && config_.server.pass_through_unknown) {
        // Rank by (priority, -weight, declaration index). Weight only breaks a
        // tie inside one priority band, which is what "same tier, prefer the
        // one I trust more" means in a hand-edited file.
        struct Ranked {
            const ProviderConfig *provider;
            std::size_t ordinal;
        };
        std::vector<Ranked> ranked;
        for (std::size_t i = 0; i < config_.providers.size(); ++i) {
            const auto &provider = config_.providers[i];
            if (!provider.enabled) {
                continue;
            }
            if (std::ranges::find(provider.models, model) != provider.models.end()) {
                ranked.push_back({&provider, i});
            }
        }
        std::ranges::sort(ranked, [](const Ranked &a, const Ranked &b) {
            if (a.provider->priority != b.provider->priority) {
                return a.provider->priority < b.provider->priority;
            }
            if (a.provider->weight != b.provider->weight) {
                return a.provider->weight > b.provider->weight;
            }
            return a.ordinal < b.ordinal;
        });
        for (const auto &entry : ranked) {
            push(*entry.provider, {}, entry.ordinal);
        }
    }

    return out;
}

bool Router::windowRunning(const ProviderHealth &state, double now_unix) {
    return now_unix < state.open_until_unix;
}

bool Router::circuitOpen(std::string_view provider, double now_unix) const {
    const auto it = health_.find(provider);
    if (it == health_.end()) {
        return false;
    }
    // Only the window, not the strike count that set it: an upstream that named
    // a Retry-After gets its window on the first failure.
    return windowRunning(it->second, now_unix);
}

std::vector<ProviderHealth> Router::health(double now_unix) const {
    std::scoped_lock lock{mutex_};
    std::vector<ProviderHealth> out;
    out.reserve(config_.providers.size());
    for (const auto &provider : config_.providers) {
        ProviderHealth view;
        view.provider = provider.id;
        if (const auto it = health_.find(provider.id); it != health_.end()) {
            view = it->second;
        }
        if (!provider.enabled) {
            // A disabled relay is neither healthy nor failing; saying so
            // avoids a green row for something that will never be called.
            view.state = ProviderHealth::State::Unknown;
        } else if (view.consecutive_failures == 0) {
            view.state = ProviderHealth::State::Healthy;
        } else if (windowRunning(view, now_unix)) {
            // The window is what makes a breaker open — a Retry-After sets one
            // on the first failure, without the strike counter ever filling.
            view.state = ProviderHealth::State::Open;
            view.cooldown_remaining = view.open_until_unix - now_unix;
        } else {
            // No window: either below the threshold, or the cooldown elapsed and
            // the next request is the probe. Reporting "degraded" rather than
            // "healthy" is honest — nothing has succeeded yet.
            view.state = ProviderHealth::State::Degraded;
            view.cooldown_remaining = 0.0;
        }
        out.push_back(std::move(view));
    }
    return out;
}

int Router::openBreakerCount(double now_unix) const {
    std::scoped_lock lock{mutex_};
    int count = 0;
    for (const auto &[id, state] : health_) {
        if (!windowRunning(state, now_unix)) {
            continue;
        }
        if (const auto *provider = config_.provider(id); provider == nullptr || !provider->enabled) {
            continue;
        }
        ++count;
    }
    return count;
}

void Router::recordSuccess(std::string_view provider, double latency_ms, double now_unix) {
    std::scoped_lock lock{mutex_};
    auto &state = slot(provider);
    state.consecutive_failures = 0;
    state.open_until_unix = 0.0;
    state.state = ProviderHealth::State::Healthy;
    if (latency_ms > 0.0) {
        state.last_error.clear();
    }
    (void)now_unix;
}

void Router::recordFailure(std::string_view provider, std::string reason, double now_unix,
                           double cooldown_hint_sec) {
    std::scoped_lock lock{mutex_};
    auto &state = slot(provider);
    ++state.consecutive_failures;
    ++state.total_failures;
    state.last_error = truncateUtf8(reason, 240);
    state.state = ProviderHealth::State::Degraded;
    // A named window counts as a full strike on its own: the upstream has told
    // us when it will be ready, and the configured cooldown is the floor under
    // it (never a reason to come back sooner than the operator allowed).
    const bool asked_to_wait = cooldown_hint_sec > 0.0;
    if (asked_to_wait || state.consecutive_failures >= config_.server.circuit_failure_threshold) {
        state.state = ProviderHealth::State::Open;
        state.open_until_unix =
            now_unix + std::max<double>(config_.server.circuit_cooldown_sec, cooldown_hint_sec);
    }
}

void Router::forget(std::string_view provider) {
    std::scoped_lock lock{mutex_};
    // `find` + `erase` rather than `erase(provider)`: the heterogeneous-erasure
    // overload is C++23 and libc++ 22 does not implement it yet, so a
    // string_view argument silently picks the (wrong) iterator overload.
    if (const auto it = health_.find(provider); it != health_.end()) {
        health_.erase(it);
    }
}

void Router::resetHealth() {
    std::scoped_lock lock{mutex_};
    health_.clear();
}

bool Router::retryableStatus(int status) {
    // 429 is the case this whole design exists for: a relay that is rate
    // limited right now is a relay the next hop can cover for. 408/409/425 are
    // transient by definition; the 5xx band is the upstream's own fault.
    if (status == 408 || status == 409 || status == 425 || status == 429) {
        return true;
    }
    if (status >= 500 && status <= 599) {
        return true;
    }
    return false;
}

} // namespace literouter
