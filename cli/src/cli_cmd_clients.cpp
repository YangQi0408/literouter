#include <CLI/CLI.hpp>
#include <iostream>
#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {
using json = nlohmann::json;
using literouter::i18n::tr;

literouter::ClientConfig* findClient(literouter::AppConfig& config, const std::string& id) {
    for (auto& client : config.clients) if (client.id == id) return &client;
    return nullptr;
}

bool failure(Context& ctx, const char* message) {
    configureColor(ctx.noColor);
    printError(std::string(tr(message)));
    ctx.exitCode = kExitConfig;
    return false;
}

json safeClient(const literouter::ClientConfig& client) {
    json keys = json::array();
    for (const auto& key : client.keys) {
        // Even --json exposes only presence, never the secret or env fallback.
        keys.push_back(json{{"id", key.id}, {"enabled", key.enabled}, {"key_set", !key.api_key.empty()}});
    }
    return json{{"id", client.id}, {"name", client.name}, {"enabled", client.enabled},
        {"keys", keys}, {"models", client.models}, {"provider_groups", client.provider_groups},
        {"requests_per_minute", client.requests_per_minute}, {"max_concurrent", client.max_concurrent},
        {"requests_per_day", client.requests_per_day}, {"tokens_per_day", client.tokens_per_day},
        {"budget_usd_per_day", client.budget_usd_per_day},
        {"token_reservation", client.token_reservation}};
}

bool persist(Context& ctx, const literouter::ConfigStore& store) {
    const auto report = literouter::validate(store.config());
    if (!report.ok()) {
        printIssues(report, std::cerr);
        ctx.exitCode = kExitConfig;
        return false;
    }
    if (auto result = store.save(); !result) {
        printError(result.error());
        ctx.exitCode = kExitFail;
        return false;
    }
    return true;
}

void changed(Context& ctx, const char* operation, const std::string& id) {
    if (ctx.json) std::println("{}", json{{"changed",operation},{"id",id}}.dump(2));
    else {
        printInfo(std::string(tr("Client configuration saved")) + ": " + id);
        printNote(std::string(tr("Reload the running proxy or enable reload_on_change to apply file edits.")), ctx.quiet);
    }
}

void list(Context& ctx, const std::string& id) {
    configureColor(ctx.noColor);
    auto store = loadStore(ctx);
    if (!store) { ctx.exitCode = kExitConfig; return; }
    if (!id.empty() && !findClient(store->config(),id)) { failure(ctx,"Client not found"); return; }
    json result = json::array();
    Table table;
    for (const auto* label : {"Client ID","Display name","STATE","Keys","RPM","Concurrency","Requests / day","Tokens / day","Budget / day"})
        table.column(std::string(tr(label)));
    for (const auto& client : store->config().clients) {
        if (!id.empty() && client.id != id) continue;
        result.push_back(safeClient(client));
        table.row({client.id,client.name,std::string(tr(client.enabled ? "enabled" : "disabled")),
            std::to_string(client.keys.size()),std::to_string(client.requests_per_minute),
            std::to_string(client.max_concurrent),std::to_string(client.requests_per_day),
            std::to_string(client.tokens_per_day),
            client.budget_usd_per_day > 0.0 ? std::format("${:.2f}", client.budget_usd_per_day)
                                            : std::string(tr("Unlimited"))});
    }
    if (ctx.json) { std::println("{}", json{{"clients",result}}.dump(2)); return; }
    if (result.empty()) printInfo(std::string(tr("No clients configured; personal access is unchanged.")));
    else table.print();
    if (!id.empty()) {
        const auto& c = *findClient(store->config(),id);
        KeyValues details;
        auto join = [](const std::vector<std::string>& values) {
            std::string value;
            for (const auto& item : values) { if (!value.empty()) value += ", "; value += item; }
            return value.empty() ? std::string(tr("Unrestricted")) : value;
        };
        details.add(std::string(tr("Allowed models")),join(c.models));
        details.add(std::string(tr("Provider groups")),join(c.provider_groups));
        details.add(std::string(tr("Token reservation")),std::to_string(c.token_reservation));
        details.add(std::string(tr("Daily budget")),
                    c.budget_usd_per_day > 0.0 ? std::format("${:.2f}", c.budget_usd_per_day)
                                               : std::string(tr("Unlimited")));
        details.print();
        Table keys;
        for (const auto* label : {"Key ID","STATE","API key"}) keys.column(std::string(tr(label)));
        for (const auto& key : c.keys) keys.row({key.id,std::string(tr(key.enabled ? "enabled" : "disabled")),
            std::string(tr(key.api_key.empty() ? "Not set" : "Configured"))});
        if (!keys.empty()) keys.print();
    }
}

struct ClientOptions {
    std::string id, name;
    std::vector<std::string> models, groups;
    int rpm=0, concurrent=0;
    std::uint64_t dailyRequests=0, dailyTokens=0, reservation=4096;
    double budgetUsd=0.0;
    bool clearModels=false, clearGroups=false;
};

void registerEdit(CLI::App& parent, Context& ctx, bool add) {
    auto opts = std::make_shared<ClientOptions>();
    auto* cmd = parent.add_subcommand(add ? "add" : "update",std::string(tr(add ? "Add a client account" : "Update client permissions and limits")));
    cmd->fallthrough();
    cmd->add_option("id",opts->id,std::string(tr("Client ID")))->required();
    auto* name = cmd->add_option("--name",opts->name,std::string(tr("Display name")));
    auto* models = cmd->add_option("--model",opts->models,std::string(tr("Allowed model, repeatable; replaces the list")));
    auto* groups = cmd->add_option("--group",opts->groups,std::string(tr("Allowed provider group, repeatable; replaces the list")));
    cmd->add_flag("--all-models",opts->clearModels,std::string(tr("Allow every model")))->excludes(models);
    cmd->add_flag("--all-groups",opts->clearGroups,std::string(tr("Allow every provider group")))->excludes(groups);
    auto* rpm = cmd->add_option("--rpm",opts->rpm,std::string(tr("Requests per minute; 0 is unlimited")))->check(CLI::NonNegativeNumber);
    auto* concurrent = cmd->add_option("--concurrent",opts->concurrent,std::string(tr("Concurrent requests; 0 is unlimited")))->check(CLI::NonNegativeNumber);
    auto* dailyRequests = cmd->add_option("--requests-per-day",opts->dailyRequests,std::string(tr("Daily request quota; 0 is unlimited")))->check(CLI::NonNegativeNumber);
    auto* dailyTokens = cmd->add_option("--tokens-per-day",opts->dailyTokens,std::string(tr("Daily token quota; 0 is unlimited")))->check(CLI::NonNegativeNumber);
    auto* reservation = cmd->add_option("--token-reservation",opts->reservation,std::string(tr("Tokens reserved per request when usage is unknown")))->check(CLI::NonNegativeNumber);
    auto* budget = cmd->add_option("--budget-usd-per-day",opts->budgetUsd,
        std::string(tr("Daily spend ceiling in US dollars; 0 is unlimited")))->check(CLI::NonNegativeNumber);
    cmd->callback([=,&ctx] {
        auto store = loadStore(ctx);
        if (!store) { ctx.exitCode=kExitConfig; return; }
        auto* target = findClient(store->config(),opts->id);
        if (add && target) { failure(ctx,"Client ID already exists"); return; }
        if (!add && !target) { failure(ctx,"Client not found"); return; }
        literouter::ClientConfig client = target ? *target : literouter::ClientConfig{};
        client.id=opts->id;
        if (name->count()) client.name=opts->name;
        if (models->count() || opts->clearModels) client.models=opts->models;
        if (groups->count() || opts->clearGroups) client.provider_groups=opts->groups;
        if (rpm->count()) client.requests_per_minute=opts->rpm;
        if (concurrent->count()) client.max_concurrent=opts->concurrent;
        if (dailyRequests->count()) client.requests_per_day=opts->dailyRequests;
        if (dailyTokens->count()) client.tokens_per_day=opts->dailyTokens;
        if (reservation->count()) client.token_reservation=opts->reservation;
        if (budget->count()) client.budget_usd_per_day=opts->budgetUsd;
        if (target) *target=std::move(client); else store->config().clients.push_back(std::move(client));
        if (persist(ctx,*store)) changed(ctx,add ? "client-added" : "client-updated",opts->id);
    });
}

void registerKey(CLI::App& parent, Context& ctx, const std::string& operation) {
    struct Options { std::string client,id,env; bool stdinKey=false; };
    auto opts=std::make_shared<Options>();
    const char* description = operation=="add" ? "Add a client key" : operation=="replace" ? "Replace a client key" :
        operation=="remove" ? "Remove a client key" : operation=="enable" ? "Enable a client key" : "Disable a client key";
    auto* cmd=parent.add_subcommand(operation,std::string(tr(description)));
    cmd->fallthrough();
    cmd->add_option("client",opts->client,std::string(tr("Client ID")))->required();
    cmd->add_option("id",opts->id,std::string(tr("Key ID")))->required();
    if (operation=="add" || operation=="replace") {
        auto* env=cmd->add_option("--key-env",opts->env,std::string(tr("Store an environment variable reference")));
        auto* stdinKey=cmd->add_flag("--key-stdin",opts->stdinKey,std::string(tr("Read the key from standard input")));
        env->excludes(stdinKey);
    }
    cmd->callback([=,&ctx] {
        auto store=loadStore(ctx);
        if (!store) {ctx.exitCode=kExitConfig;return;}
        auto* client=findClient(store->config(),opts->client);
        if (!client) {failure(ctx,"Client not found");return;}
        auto found=std::ranges::find(client->keys,opts->id,&literouter::ClientKeyConfig::id);
        if (operation=="add" && found!=client->keys.end()) {failure(ctx,"Key ID already exists");return;}
        if (operation!="add" && found==client->keys.end()) {failure(ctx,"Key not found");return;}
        if (operation=="add" || operation=="replace") {
            std::string raw;
            if (!opts->env.empty()) {
                if (!((opts->env[0]>='A' && opts->env[0]<='Z') || (opts->env[0]>='a' && opts->env[0]<='z') || opts->env[0]=='_') ||
                    opts->env.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos) {
                    failure(ctx,"Invalid environment variable name");return;
                }
                raw="${"+opts->env+"}";
            } else if (opts->stdinKey) {
                if (!std::getline(std::cin,raw) || raw.empty()) {failure(ctx,"No key received on standard input");return;}
                if (!raw.empty() && raw.back()=='\r') raw.pop_back();
            } else {failure(ctx,"Choose --key-env or --key-stdin");return;}
            if (operation=="add") client->keys.push_back({opts->id,raw,true});
            else found->api_key=raw;
        } else if (operation=="remove") client->keys.erase(found);
        else found->enabled=operation=="enable";
        if (persist(ctx,*store)) changed(ctx,"client-key-updated",opts->client);
    });
}

void usage(Context& ctx, const std::string& id) {
    configureColor(ctx.noColor);
    auto store=loadStore(ctx);
    if (!store) {ctx.exitCode=kExitConfig;return;}
    if (!id.empty() && !findClient(store->config(),id)) {failure(ctx,"Client not found");return;}
    const auto status=literouter::fetchStatus(baseUrlOf(store->config()),store->config().server.api_key);
    if (!status.reachable) {printError(status.error);ctx.exitCode=kExitUnreachable;return;}
    json result=json::array();
    Table table;
    for (const auto* label : {"Client ID","REQUESTS","SUCCESS","FAILURES","IN FLIGHT","Requests today","Tokens today","Reserved tokens","Spend today","Prompt / completion tokens","Estimated cost"})
        table.column(std::string(tr(label)));
    for (const auto& client:store->config().clients) {
        if (!id.empty() && id!=client.id) continue;
        literouter::ClientUsage u;
        u.client=client.id;
        for (const auto& item:status.snapshot.clients) if (item.client==client.id) {u=item;break;}
        result.push_back(json::parse(literouter::toJsonString(u)));
        const std::string spend = client.budget_usd_per_day > 0.0
            ? std::format("${:.4f} / ${:.2f}", u.cost_today, client.budget_usd_per_day)
            : std::format("${:.4f}", u.cost_today);
        table.row({u.client,std::to_string(u.requests),std::to_string(u.successes),std::to_string(u.failures),
            std::to_string(u.active_requests),std::to_string(u.requests_today),std::to_string(u.tokens_today),
            std::to_string(u.reserved_tokens),spend,
            literouter::humanCount(u.tokens_prompt)+" / "+literouter::humanCount(u.tokens_completion),std::format("${:.4f}",u.cost_usd)});
    }
    if (ctx.json) std::println("{}",json{{"clients",result}}.dump(2));
    else {table.print();printNote(std::string(tr("Daily quotas and budgets reset at 00:00 UTC. Token totals include reservations.")),ctx.quiet);}
}
} // namespace

void register_clients(CLI::App& root, Context& ctx) {
    auto* parent=root.add_subcommand("clients",std::string(tr("Manage client accounts, keys, permissions and quotas")));
    parent->require_subcommand(1);parent->fallthrough();
    for (const auto* name : {"list","show"}) {
        auto id=std::make_shared<std::string>();
        auto* cmd=parent->add_subcommand(name,std::string(tr("Show client configuration without secrets")));
        cmd->fallthrough();
        auto* option=cmd->add_option("id",*id,std::string(tr("Client ID")));
        if (std::string(name)=="show") option->required();
        cmd->callback([id,&ctx]{list(ctx,*id);});
    }
    registerEdit(*parent,ctx,true);registerEdit(*parent,ctx,false);
    for (const auto* operation : {"remove","enable","disable"}) {
        auto id=std::make_shared<std::string>();
        const std::string op=operation;
        const char* description=op=="remove" ? "Remove a client account" : op=="enable" ? "Enable a client account" : "Disable a client account";
        auto* cmd=parent->add_subcommand(op,std::string(tr(description)));
        cmd->fallthrough();cmd->add_option("id",*id,std::string(tr("Client ID")))->required();
        cmd->callback([id,op,&ctx] {
            auto store=loadStore(ctx);
            if (!store) {ctx.exitCode=kExitConfig;return;}
            auto* client=findClient(store->config(),*id);
            if (!client) {failure(ctx,"Client not found");return;}
            if (op=="remove") std::erase_if(store->config().clients,[&](const auto& item){return item.id==*id;});
            else client->enabled=op=="enable";
            if (persist(ctx,*store)) changed(ctx,op=="remove" ? "client-removed" : "client-updated",*id);
        });
    }
    auto* keys=parent->add_subcommand("keys",std::string(tr("Manage client keys")));
    keys->require_subcommand(1);keys->fallthrough();
    for (const auto* operation:{"add","replace","remove","enable","disable"}) registerKey(*keys,ctx,operation);
    auto id=std::make_shared<std::string>();
    auto* stats=parent->add_subcommand("usage",std::string(tr("Show live client usage and daily quotas")));
    stats->fallthrough();stats->add_option("id",*id,std::string(tr("Client ID")));
    stats->callback([id,&ctx]{usage(ctx,*id);});
}
} // namespace lrcli
