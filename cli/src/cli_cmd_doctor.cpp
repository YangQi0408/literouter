#include <CLI/CLI.hpp>

#include <cstdlib>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"
#include "cli_json.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

enum class StepStatus { Pass, Warn, Fail };

struct Step {
    int number = 0;
    std::string title;
    StepStatus status = StepStatus::Pass;
    std::string detail;
    std::vector<std::string> lines;
    std::vector<std::string> hints;
};

const char *statusName(StepStatus status) {
    switch (status) {
    case StepStatus::Pass: return "pass";
    case StepStatus::Warn: return "warn";
    case StepStatus::Fail: return "fail";
    }
    return "pass";
}

std::string statusBadge(StepStatus status) {
    switch (status) {
    case StepStatus::Pass: return paint(" PASS ", "42;30");
    case StepStatus::Warn: return paint(" WARN ", "43;30");
    case StepStatus::Fail: return paint(" FAIL ", "41;97");
    }
    return {};
}

void reportStep(const Step &step) {
    std::println("{} {} {}", statusBadge(step.status), bold(std::to_string(step.number) + "."),
                 step.title);
    if (!step.detail.empty()) {
        std::println("       {}", step.detail);
    }
    for (const auto &line : step.lines) {
        std::println("       {}", line);
    }
    for (const auto &hint : step.hints) {
        std::println("       {}", paint("fix: " + hint, "33"));
    }
    std::println("");
}

bool probeOk(const literouter::ProviderProbe &probe) {
    // Mirrors the server's own accounting: a non-retryable 4xx is the caller's
    // bad request, so a relay that returns one is still reachable/a healthy
    // relay. A transport failure or a retryable status is a real problem.
    return probe.reachable && !literouter::Router::retryableStatus(probe.status);
}

void runDoctor(Context &ctx) {
    configureColor(ctx.noColor);
    const std::filesystem::path path = configPathFor(ctx);

    std::vector<Step> steps;
    literouter::AppConfig config;
    bool haveConfig = false;

    // 1. config file present, readable, parses
    {
        Step step{.number = 1, .title = std::string(literouter::i18n::tr("config file"))};
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            step.status = StepStatus::Warn;
            step.detail = std::format("`{}` does not exist", literouter::pathToUtf8(path));
            step.hints.push_back("run `literouter config init` to write the seed");
        } else if (auto loaded = literouter::ConfigStore::load(path); !loaded) {
            step.status = StepStatus::Fail;
            step.detail = std::format("`{}` did not load: {}", literouter::pathToUtf8(path),
                                      loaded.error());
            step.hints.push_back(
                std::format("fix the JSON, or rewrite it with `literouter config init --force`"));
        } else {
            config = loaded->config();
            haveConfig = true;
            step.status = StepStatus::Pass;
            step.detail = std::format("`{}` parsed", literouter::pathToUtf8(path));
        }
        steps.push_back(std::move(step));
    }
    if (!haveConfig) {
        config = literouter::ConfigStore::seedDefault();
    }

    // 2. validate() report
    {
        Step step{.number = 2, .title = std::string(literouter::i18n::tr("validation"))};
        const literouter::ValidationReport report = literouter::validate(config);
        if (!report.ok()) {
            step.status = StepStatus::Fail;
            step.detail = std::format("{}", report.summary());
            for (const auto &issue : report.issues) {
                if (issue.level == literouter::ValidationIssue::Level::Error) {
                    step.lines.push_back(colorLevel("error", "error") + " " + issue.path + ": " +
                                         issue.message);
                }
            }
            step.hints.push_back("run `literouter config validate` for the full report");
        } else if (report.count(literouter::ValidationIssue::Level::Warning) > 0) {
            step.status = StepStatus::Warn;
            step.detail = std::format("{}", report.summary());
            step.hints.push_back("run `literouter config validate` to see the warnings");
        } else {
            step.status = StepStatus::Pass;
            step.detail = "no issues";
        }
        steps.push_back(std::move(step));
    }

    // 3. environment-variable api_key references resolve in this shell
    {
        Step step{.number = 3, .title = std::string(literouter::i18n::tr("secret references"))};
        std::vector<std::string> missing;
        for (const auto &provider : config.providers) {
            if (!literouter::isSecretReference(provider.api_key)) {
                continue;
            }
            const std::string name = literouter::secretReferenceName(provider.api_key);
            if (name.empty()) {
                step.lines.push_back(colorLevel("error", "error") + " " + provider.id +
                                     ": malformed reference `" + provider.api_key + "`");
                continue;
            }
            if (std::getenv(name.c_str()) == nullptr) {
                missing.push_back(name);
            } else {
                step.lines.push_back(colorEnabled(true, "ok") + " " + provider.id + " -> $" + name);
            }
        }
        if (!missing.empty()) {
            step.status = StepStatus::Fail;
            step.detail = std::format("{} environment variable(s) are unset here",
                                      missing.size());
            for (const auto &name : missing) {
                step.lines.push_back(colorLevel("error", "error") + " $" + name + " is not set");
            }
            step.hints.push_back(std::format(
                "export the variable(s), or change the key in `{}`", literouter::pathToUtf8(path)));
        } else {
            step.status = StepStatus::Pass;
            step.detail = "every `${VAR}` key resolves";
        }
        steps.push_back(std::move(step));
    }

    // 4. probe every enabled relay
    std::map<std::string, literouter::ProviderProbe> probes;
    {
        Step step{.number = 4, .title = std::string(literouter::i18n::tr("enabled relays"))};
        // Stated before any probe result, because "certificate rejected" and
        // "this process has no trust store" look identical from a relay's point
        // of view and have completely different fixes. mcpp builds OpenSSL from
        // source, so its compiled-in default verify path points at that build
        // tree rather than at this machine's roots — which is why the
        // resolution is reported rather than assumed.
        const std::string ca = literouter::caBundleSummary();
        step.lines.push_back(ca.empty()
                                 ? dim("tls trust store    library defaults (no system "
                                       "bundle found)")
                                 : dim("tls trust store    " + ca));
#ifdef _WIN32
        if (ca.empty()) {
            step.hints.push_back(
                "if HTTPS requests fail on Windows, set LITEROUTER_CA_BUNDLE to your Git/curl ca-bundle.crt");
        }
#endif
        int failures = 0;
        int checked = 0;
        for (const auto &provider : config.providers) {
            if (!provider.enabled) {
                continue;
            }
            ++checked;
            const literouter::ProviderProbe probe = literouter::probeProvider(provider);
            probes.emplace(provider.id, probe);
            const bool ok = probeOk(probe);
            if (!ok) {
                ++failures;
            }
            step.lines.push_back(std::format(
                "{} {}  {}  {}  {} model(s)  {}",
                ok ? colorEnabled(true, "ok  ") : colorLevel("error", "fail"),
                provider.id,
                probe.status == 0 ? "---" : colorStatus(probe.status, std::to_string(probe.status)),
                literouter::humanMillis(probe.latency_ms),
                probe.models.size(),
                probe.detail));
        }
        if (checked == 0) {
            step.status = StepStatus::Warn;
            step.detail = "no enabled relay to probe";
            step.hints.push_back("enable one with `literouter providers enable <id>`");
        } else if (failures > 0) {
            step.status = StepStatus::Fail;
            step.detail = std::format("{} of {} relay(s) did not answer cleanly", failures, checked);
            step.hints.push_back("check the base_url, the key, and reachability of each relay");
        } else {
            step.status = StepStatus::Pass;
            step.detail = std::format("{} relay(s) answered", checked);
        }
        steps.push_back(std::move(step));
    }

    // 5. effective route table with each hop's reachability
    {
        Step step{.number = 5, .title = std::string(literouter::i18n::tr("route table"))};
        bool anyFail = false;
        bool anyWarn = false;
        int routes = 0;
        for (const auto &route : config.routes) {
            if (!route.enabled) {
                continue;
            }
            ++routes;
            std::string chain;
            for (std::size_t i = 0; i < route.targets.size(); ++i) {
                if (i > 0) {
                    chain += " → ";
                }
                const std::string &id = route.targets[i].provider;
                const literouter::ProviderConfig *provider = config.provider(id);
                std::string state;
                if (provider == nullptr) {
                    state = colorLevel("error", "missing");
                    anyFail = true;
                } else if (!provider->enabled) {
                    state = dim("disabled");
                    anyWarn = true;
                } else if (const auto it = probes.find(id); it != probes.end()) {
                    if (probeOk(it->second)) {
                        state = colorEnabled(true, "reachable");
                    } else {
                        state = colorLevel("error", "unreachable");
                        anyFail = true;
                    }
                } else {
                    state = dim("unknown");
                }
                chain += id + "(" + state + ")";
            }
            step.lines.push_back(std::format("{}: {}", route.model, chain));
        }
        if (anyFail) {
            step.status = StepStatus::Fail;
            step.detail = "at least one hop can never serve";
        } else if (anyWarn) {
            step.status = StepStatus::Warn;
            step.detail = std::string(literouter::i18n::tr("at least one hop is disabled and will be skipped"));
        } else if (routes == 0) {
            step.status = StepStatus::Warn;
            step.detail = "no enabled route";
            step.hints.push_back("add one with `literouter routes add <model> --target <relay>`");
        } else {
            step.status = StepStatus::Pass;
            step.detail = std::format("{} route(s) resolved", routes);
        }
        steps.push_back(std::move(step));
    }

    // 6. is something already listening on the configured host:port?
    {
        Step step{.number = 6, .title = std::string(literouter::i18n::tr("listener"))};
        const std::string url = baseUrlOf(config);
        const literouter::AdminStatus status = literouter::fetchStatus(url, config.server.api_key);
        if (status.reachable) {
            step.status = StepStatus::Pass;
            step.detail = std::format("literouter {} is already listening at {}", 
                                      status.snapshot.version.empty() ? "" : status.snapshot.version,
                                      url);
            step.lines.push_back("this is fine if you meant to attach to it with `status`/`logs`");
        } else if (status.error.find("HTTP ") != std::string::npos) {
            step.status = StepStatus::Fail;
            step.detail =
                std::format("something that is not literouter answered at {}: {}", url, status.error);
            step.hints.push_back(std::format(
                "change `server.port` in `{}`, or stop the process on port {}",
                literouter::pathToUtf8(path), config.server.port));
        } else {
            step.status = StepStatus::Pass;
            step.detail = std::format("nothing is listening at {} ({}) — the port is free", url,
                                      status.error);
        }
        steps.push_back(std::move(step));
    }

    bool anyFail = false;
    if (ctx.json) {
        json array = json::array();
        for (const auto &step : steps) {
            if (step.status == StepStatus::Fail) {
                anyFail = true;
            }
            json node = json::object();
            node["step"] = step.number;
            node["title"] = step.title;
            node["status"] = statusName(step.status);
            node["detail"] = step.detail;
            node["lines"] = step.lines;
            node["hints"] = step.hints;
            array.push_back(std::move(node));
        }
        json root = json::object();
        root["ok"] = !anyFail;
        root["path"] = literouter::pathToUtf8(path);
        root["steps"] = std::move(array);
        std::println("{}", dumpJson(root, 2));
    } else {
        std::println("{}",
                     bold(std::format("literouter doctor — {}", literouter::pathToUtf8(path))));
        std::println("");
        for (const auto &step : steps) {
            if (step.status == StepStatus::Fail) {
                anyFail = true;
            }
            reportStep(step);
        }
        if (anyFail) {
            printError(paint(std::string(literouter::i18n::tr("doctor found at least one FAIL")), "31;1"));
            ctx.exitCode = kExitFail;
        } else {
            printInfo(paint(std::string(literouter::i18n::tr("all checks passed")), "32"));
        }
    }
}

} // namespace

void register_doctor(CLI::App &root, Context &ctx) {
    CLI::App *sub = root.add_subcommand(
        "doctor", std::string(literouter::i18n::tr("Diagnose the config and the environment")));
    sub->fallthrough();
    sub->callback([&ctx] { runDoctor(ctx); });
}

} // namespace lrcli
