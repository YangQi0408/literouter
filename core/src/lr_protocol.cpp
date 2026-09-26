// Protocol adapters: bidirectional translation between OpenAI format and
// upstream provider protocols (Anthropic Claude Messages API, Google Gemini,
// and OpenAI Responses API).

module;

#include <httplib.h>

module literouter.core;

import std;
import nlohmann.json;

#include "lr_dump.h"

namespace literouter {

namespace {

using json = nlohmann::json;

bool ciEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

bool isAnthropic(const ProviderConfig &p) {
    return ciEqual(p.protocol, "anthropic");
}

bool isGemini(const ProviderConfig &p) {
    return ciEqual(p.protocol, "gemini");
}

bool isOpenAiResponses(const ProviderConfig &p) {
    return ciEqual(p.protocol, "openai_responses");
}

bool isAzure(const ProviderConfig &p) {
    return ciEqual(p.protocol, "azure");
}

bool isVertex(const ProviderConfig &p) {
    return ciEqual(p.protocol, "vertex");
}

bool isOllama(const ProviderConfig &p) {
    return ciEqual(p.protocol, "ollama");
}

bool isBedrock(const ProviderConfig &p) {
    return ciEqual(p.protocol, "bedrock");
}

// The Azure api-version a relay gets when it names none. A date rather than a
// symbolic alias: Azure retires preview versions, and a config that silently
// follows a moving alias is a config whose behaviour changes without an edit.
constexpr std::string_view kAzureApiVersion = "2024-10-21";

// The text of a chat message whatever shape its `content` arrived in. Every
// adapter that has to fold an OpenAI message into a single-string field needs
// this, and writing it once is what keeps them from disagreeing about what an
// array of parts means.
std::string contentAsText(const json &message) {
    if (!message.is_object() || !message.contains("content")) {
        return {};
    }
    const auto &content = message["content"];
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (content.is_array()) {
        std::string out;
        for (const auto &part : content) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                out += part["text"].get<std::string>();
            }
        }
        return out;
    }
    return {};
}

// A data URL's media type and payload, for the protocols that want the bytes
// inline rather than as a URL.
bool splitDataUrl(std::string_view url, std::string &mime, std::string &base64) {
    if (!startsWith(url, "data:")) {
        return false;
    }
    const auto comma = url.find(";base64,");
    if (comma == std::string_view::npos) {
        return false;
    }
    mime = std::string{url.substr(5, comma - 5)};
    base64 = std::string{url.substr(comma + 8)};
    return true;
}

// Responses and Chat share concepts, but their messages, content parts and
// function definitions are different wire contracts. Keep these conversions
// together so a tool call and the next turn's result retain the same call id.
json responseContentToChat(const json &content) {
    if (!content.is_array()) return content;
    json parts = json::array();
    for (const auto &part : content) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", "");
        if (type == "input_text" || type == "output_text" || type == "text") {
            parts.push_back({{"type", "text"}, {"text", part.value("text", "")}});
        } else if (type == "input_image") {
            json image = {{"url", part.value("image_url", "")}};
            if (part.contains("detail")) image["detail"] = part["detail"];
            parts.push_back({{"type", "image_url"}, {"image_url", std::move(image)}});
        } else if (type == "input_file") {
            json file = json::object();
            for (const char *key : {"file_id", "file_data", "filename"}) {
                if (part.contains(key)) file[key] = part[key];
            }
            parts.push_back({{"type", "file"}, {"file", std::move(file)}});
        }
    }
    return parts;
}

json chatContentToResponse(const json &content, bool assistant) {
    if (!content.is_array()) return content;
    json parts = json::array();
    for (const auto &part : content) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", "");
        if (type == "text") {
            json converted = {{"type", assistant ? "output_text" : "input_text"}, {"text", part.value("text", "")}};
            if (assistant) converted["annotations"] = json::array();
            parts.push_back(std::move(converted));
        } else if (type == "image_url" && part.contains("image_url") &&
                   part["image_url"].is_object()) {
            const auto &image = part["image_url"];
            json converted = {{"type", "input_image"}, {"image_url", image.value("url", "")}};
            if (image.contains("detail")) converted["detail"] = image["detail"];
            parts.push_back(std::move(converted));
        } else if (type == "file" && part.contains("file") && part["file"].is_object()) {
            json converted = part["file"];
            converted["type"] = "input_file";
            parts.push_back(std::move(converted));
        }
    }
    return parts;
}

json responseUsageToChat(const json &usage) {
    if (!usage.is_object()) return json::object();
    const auto input = usage.value("input_tokens", std::uint64_t{0});
    const auto output = usage.value("output_tokens", std::uint64_t{0});
    json result = {{"prompt_tokens", input}, {"completion_tokens", output},
                   {"total_tokens", usage.value("total_tokens", input + output)}};
    if (usage.contains("input_tokens_details")) result["prompt_tokens_details"] = usage["input_tokens_details"];
    if (usage.contains("output_tokens_details")) result["completion_tokens_details"] = usage["output_tokens_details"];
    return result;
}

json chatUsageToResponse(const json &usage) {
    if (!usage.is_object()) return json::object();
    const auto input = usage.value("prompt_tokens", std::uint64_t{0});
    const auto output = usage.value("completion_tokens", std::uint64_t{0});
    json result = {{"input_tokens", input}, {"output_tokens", output},
                   {"total_tokens", usage.value("total_tokens", input + output)}};
    if (usage.contains("prompt_tokens_details")) result["input_tokens_details"] = usage["prompt_tokens_details"];
    if (usage.contains("completion_tokens_details")) result["output_tokens_details"] = usage["completion_tokens_details"];
    return result;
}

json responseFunctionToChat(const json &item) {
    return {{"id", item.value("call_id", item.value("id", "call_" + hexId(8)))},
            {"type", "function"},
            {"function", {{"name", item.value("name", "")},
                          {"arguments", item.value("arguments", "")}}}};
}

// Only fields with the same meaning and spelling belong in this copy. The
// protocol-specific ones below are translated, never forwarded as unknown keys.
void copyResponseOptions(const json &source, json &target) {
    for (const char *key : {"temperature", "top_p", "parallel_tool_calls", "metadata", "store", "user"}) {
        if (source.contains(key)) target[key] = source[key];
    }
}

json chatRequestToResponse(const json &req, std::string_view model, bool stream) {
    json out = {{"model", std::string{model}}, {"stream", stream}};
    json input = json::array();
    if (req.contains("messages") && req["messages"].is_array()) {
        for (const auto &message : req["messages"]) {
            if (!message.is_object()) continue;
            const std::string role = message.value("role", "user");
            if (role == "tool" || role == "function") {
                input.push_back({{"type", "function_call_output"},
                                 {"call_id", message.value("tool_call_id", message.value("name", ""))},
                                 {"output", contentAsText(message)}});
                continue;
            }
            if (message.contains("content") && !message["content"].is_null() &&
                (!message["content"].empty() || !message.contains("tool_calls"))) {
                json converted = {{"role", role}, {"content", chatContentToResponse(message["content"], role == "assistant")}};
                if (role == "assistant" && converted["content"].is_array()) {
                    converted["type"] = "message";
                    converted["id"] = "msg_" + hexId(12);
                    converted["status"] = "completed";
                }
                input.push_back(std::move(converted));
            }
            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto &call : message["tool_calls"]) {
                    if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) continue;
                    const auto &fn = call["function"];
                    input.push_back({{"type", "function_call"}, {"call_id", call.value("id", "")},
                                     {"name", fn.value("name", "")}, {"arguments", fn.value("arguments", "")}});
                }
            }
        }
    }
    out["input"] = std::move(input);
    copyResponseOptions(req, out);
    if (req.contains("max_completion_tokens")) out["max_output_tokens"] = req["max_completion_tokens"];
    else if (req.contains("max_tokens")) out["max_output_tokens"] = req["max_tokens"];
    if (req.contains("reasoning_effort")) out["reasoning"] = {{"effort", req["reasoning_effort"]}};
    if (req.contains("tools") && req["tools"].is_array()) {
        json tools = json::array();
        for (const auto &tool : req["tools"]) {
            if (!tool.is_object() || tool.value("type", "") != "function" ||
                !tool.contains("function") || !tool["function"].is_object()) continue;
            json converted = tool["function"];
            converted["type"] = "function";
            tools.push_back(std::move(converted));
        }
        out["tools"] = std::move(tools);
    }
    if (req.contains("tool_choice")) {
        const auto &choice = req["tool_choice"];
        if (choice.is_object() && choice.value("type", "") == "function" &&
            choice.contains("function") && choice["function"].is_object()) {
            out["tool_choice"] = {{"type", "function"}, {"name", choice["function"].value("name", "")}};
        } else if (choice.is_string()) out["tool_choice"] = choice;
    }
    if (req.contains("response_format") && req["response_format"].is_object()) {
        const auto &format = req["response_format"];
        if (format.value("type", "") == "json_schema" && format.contains("json_schema") &&
            format["json_schema"].is_object()) {
            json converted = format["json_schema"];
            converted["type"] = "json_schema";
            out["text"] = {{"format", std::move(converted)}};
        } else out["text"] = {{"format", format}};
    }
    return out;
}

} // namespace

WireShape wireShapeOf(std::string_view protocol) {
    const std::string proto = toLower(protocol);
    if (proto == "anthropic") return WireShape::Anthropic;
    if (proto == "gemini" || proto == "vertex") return WireShape::Gemini;
    if (proto == "openai_responses") return WireShape::Responses;
    if (proto == "ollama") return WireShape::Ollama;
    if (proto == "bedrock") return WireShape::Bedrock;
    // "openai", the two historical aliases, "azure" and anything unrecognised.
    // An unknown protocol falls back to OpenAI's shape because that is what the
    // overwhelming majority of relays speak, and because the alternative —
    // refusing to send anything — turns a typo into an outage.
    return WireShape::OpenAi;
}

std::string_view wireShapeName(WireShape shape) {
    switch (shape) {
    case WireShape::OpenAi: return "openai";
    case WireShape::Anthropic: return "anthropic";
    case WireShape::Gemini: return "gemini";
    case WireShape::Responses: return "openai_responses";
    case WireShape::Ollama: return "ollama";
    case WireShape::Bedrock: return "bedrock";
    }
    return "openai";
}

namespace {

// The parts of resolveChatPath/resolveModelsPath that are not a protocol's
// default path: the ones that are computed from the relay's own settings.
std::string vertexPath(const ProviderConfig &provider, std::string_view upstream_model,
                       bool stream) {
    const std::string version = provider.api_version.empty() ? "v1" : provider.api_version;
    const std::string location = provider.region.empty() ? "us-central1" : provider.region;
    const std::string base = std::format("/{}/projects/{}/locations/{}/publishers/google/models/{}",
                                         version, provider.project, location, upstream_model);
    return stream ? base + ":streamGenerateContent?alt=sse" : base + ":generateContent";
}

bool hasCustomChatPath(const ProviderConfig &provider) {
    return !provider.chat_path.empty() && provider.chat_path != "/chat/completions";
}

} // namespace

std::string resolveChatPath(const ProviderConfig &provider,
                            std::string_view upstream_model,
                            bool stream) {
    const WireShape shape = wireShapeOf(provider.protocol);
    if (shape == WireShape::Gemini) {
        if (hasCustomChatPath(provider)) {
            return provider.chat_path;
        }
        if (isVertex(provider)) {
            return vertexPath(provider, upstream_model, stream);
        }
        if (stream) {
            return std::format("/v1beta/models/{}:streamGenerateContent?alt=sse", upstream_model);
        }
        return std::format("/v1beta/models/{}:generateContent", upstream_model);
    }
    if (shape == WireShape::Anthropic) {
        return hasCustomChatPath(provider) ? provider.chat_path : std::string{"/v1/messages"};
    }
    if (shape == WireShape::Responses) {
        return hasCustomChatPath(provider) ? provider.chat_path : std::string{"/v1/responses"};
    }
    if (shape == WireShape::OpenAi) {
        if (isAzure(provider)) {
            if (hasCustomChatPath(provider)) {
                return provider.chat_path;
            }
            // Azure names the model in the path as a *deployment*, not in the
            // body, and requires an api-version query — the two reasons it
            // cannot share the plain OpenAI path.
            const std::string version = provider.api_version.empty()
                                            ? std::string{kAzureApiVersion}
                                            : provider.api_version;
            return std::format("/openai/deployments/{}/chat/completions?api-version={}",
                               upstream_model, version);
        }
        return provider.chat_path;
    }
    if (shape == WireShape::Ollama) {
        return hasCustomChatPath(provider) ? provider.chat_path : std::string{"/api/chat"};
    }
    if (shape == WireShape::Bedrock) {
        if (hasCustomChatPath(provider)) {
            return provider.chat_path;
        }
        // The Converse API is one endpoint with its own body, not the OpenAI
        // schema: /model/{id}/converse is the only shape that exists.
        return stream ? std::format("/model/{}/converse-stream", upstream_model)
                      : std::format("/model/{}/converse", upstream_model);
    }
    return provider.chat_path;
}

std::string resolveModelsPath(const ProviderConfig &provider) {
    const WireShape shape = wireShapeOf(provider.protocol);
    if (shape == WireShape::Gemini) {
        if (isVertex(provider)) {
            const std::string version = provider.api_version.empty() ? "v1" : provider.api_version;
            const std::string location = provider.region.empty() ? "us-central1" : provider.region;
            return std::format("/{}/projects/{}/locations/{}/publishers/google/models", version,
                               provider.project, location);
        }
        return "/v1beta/models";
    }
    if (shape == WireShape::Anthropic) {
        return "/v1/models";
    }
    if (isAzure(provider)) {
        const std::string version =
            provider.api_version.empty() ? std::string{kAzureApiVersion} : provider.api_version;
        return std::format("/openai/models?api-version={}", version);
    }
    if (shape == WireShape::Ollama) {
        // Ollama lists local models on /api/tags, and the reply is
        // {"models":[{"name":...}]} — a shape parseModelIds already reads.
        return "/api/tags";
    }
    if (shape == WireShape::Bedrock) {
        // Bedrock's model catalogue lives on a different host and a different
        // signing service (bedrock, not bedrock-runtime), so a probe cannot
        // list it. It still reports reachability, which is the useful half.
        return "/models";
    }
    return "/models";
}

std::string adaptChatRequest(const ProviderConfig &provider,
                             std::string_view upstream_model,
                             std::string_view openai_request_json,
                             bool stream) {
    const json req = json::parse(openai_request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return std::string{openai_request_json};
    }

    // Dispatched on the wire shape rather than the protocol name: `vertex` is
    // Gemini's body, and `azure` is OpenAI's. A name comparison here would send
    // an OpenAI body to Vertex and an unadapted one to a plain OpenAI relay.
    const WireShape shape = wireShapeOf(provider.protocol);

    if (shape == WireShape::Anthropic) {
        json anthropic = json::object();
        anthropic["model"] = std::string{upstream_model};
        anthropic["stream"] = stream;

        if (req.contains("max_tokens") && req["max_tokens"].is_number()) {
            anthropic["max_tokens"] = req["max_tokens"];
        } else if (req.contains("max_completion_tokens") && req["max_completion_tokens"].is_number()) {
            anthropic["max_tokens"] = req["max_completion_tokens"];
        } else {
            anthropic["max_tokens"] = 4096;
        }

        if (req.contains("temperature")) anthropic["temperature"] = req["temperature"];
        if (req.contains("top_p")) anthropic["top_p"] = req["top_p"];
        if (req.contains("top_k")) anthropic["top_k"] = req["top_k"];

        std::string system_str;
        json raw_msgs = json::array();

        if (req.contains("messages") && req["messages"].is_array()) {
            for (const auto &m : req["messages"]) {
                if (!m.is_object()) continue;
                std::string role = m.value("role", "user");
                if (role == "system") {
                    if (!system_str.empty()) system_str += "\n\n";
                    if (m.contains("content")) {
                        if (m["content"].is_string()) {
                            system_str += m["content"].get<std::string>();
                        } else if (m["content"].is_array()) {
                            for (const auto &p : m["content"]) {
                                if (p.value("type", "") == "text" && p.contains("text")) {
                                    system_str += p["text"].get<std::string>();
                                }
                            }
                        }
                    }
                } else {
                    json msg = json::object();
                    if (role == "tool" || role == "function") {
                        msg["role"] = "user";
                        json content_arr = json::array();
                        json tool_res = json::object();
                        tool_res["type"] = "tool_result";
                        tool_res["tool_use_id"] = m.value("tool_call_id", m.value("name", "call_0"));
                        tool_res["content"] =
                            m.contains("content")
                                ? (m["content"].is_string() ? m["content"].get<std::string>()
                                                            : dumpJson(m["content"]))
                                : "";
                        content_arr.push_back(tool_res);
                        msg["content"] = content_arr;
                    } else if (role == "assistant" && m.contains("tool_calls") && m["tool_calls"].is_array() &&
                               !m["tool_calls"].empty()) {
                        msg["role"] = "assistant";
                        json content_arr = json::array();
                        if (m.contains("content") && m["content"].is_string() && !m["content"].get<std::string>().empty()) {
                            content_arr.push_back({{"type", "text"}, {"text", m["content"].get<std::string>()}});
                        }
                        for (const auto &tc : m["tool_calls"]) {
                            json tool_use = json::object();
                            tool_use["type"] = "tool_use";
                            tool_use["id"] = tc.value("id", "call_" + hexId(8));
                            if (tc.contains("function") && tc["function"].is_object()) {
                                tool_use["name"] = tc["function"].value("name", "");
                                std::string args_str = tc["function"].value("arguments", "{}");
                                json parsed_args = json::parse(args_str, nullptr, false);
                                tool_use["input"] = parsed_args.is_discarded() ? json::object() : parsed_args;
                            }
                            content_arr.push_back(tool_use);
                        }
                        msg["content"] = content_arr;
                    } else {
                        msg["role"] = (role == "assistant") ? "assistant" : "user";
                        if (m.contains("content")) {
                            if (m["content"].is_string()) {
                                msg["content"] = m["content"].get<std::string>();
                            } else if (m["content"].is_array()) {
                                json content_arr = json::array();
                                for (const auto &p : m["content"]) {
                                    std::string ptype = p.value("type", "text");
                                    if (ptype == "text") {
                                        content_arr.push_back({{"type", "text"}, {"text", p.value("text", "")}});
                                    } else if (ptype == "reasoning_content") {
                                        // Deliberately not turned into a `thinking`
                                        // block: extended thinking is only accepted
                                        // back WITH the signature the provider issued
                                        // for it, and inventing one turns a request
                                        // that would have succeeded into a 400. The
                                        // reasoning is dropped here instead — see
                                        // adaptChatToAnthropic() for the direction
                                        // where no signature has to be checked.
                                    } else if (ptype == "image_url" && p.contains("image_url")) {
                                        std::string url = p["image_url"].value("url", "");
                                        if (startsWith(url, "data:") && url.find(";base64,") != std::string::npos) {
                                            auto comma = url.find(";base64,");
                                            std::string mime = url.substr(5, comma - 5);
                                            std::string b64 = url.substr(comma + 8);
                                            content_arr.push_back({
                                                {"type", "image"},
                                                {"source", {
                                                    {"type", "base64"},
                                                    {"media_type", mime},
                                                    {"data", b64}
                                                }}
                                            });
                                        }
                                    }
                                }
                                msg["content"] = content_arr;
                            }
                        } else {
                            msg["content"] = "";
                        }
                    }
                    raw_msgs.push_back(msg);
                }
            }
        }

        if (!system_str.empty()) {
            anthropic["system"] = system_str;
        }

        json merged_msgs = json::array();
        for (const auto &m : raw_msgs) {
            if (merged_msgs.empty()) {
                if (m["role"] == "assistant") {
                    merged_msgs.push_back({{"role", "user"}, {"content", "..."}});
                }
                merged_msgs.push_back(m);
            } else if (merged_msgs.back()["role"] == m["role"]) {
                auto &prev = merged_msgs.back();
                if (prev["content"].is_string() && m["content"].is_string()) {
                    prev["content"] = prev["content"].get<std::string>() + "\n\n" + m["content"].get<std::string>();
                } else {
                    json prev_arr = prev["content"].is_array()
                        ? prev["content"]
                        : json::array({ {{"type", "text"}, {"text", prev["content"].get<std::string>()}} });
                    json curr_arr = m["content"].is_array()
                        ? m["content"]
                        : json::array({ {{"type", "text"}, {"text", m["content"].get<std::string>()}} });
                    for (const auto &item : curr_arr) {
                        prev_arr.push_back(item);
                    }
                    prev["content"] = prev_arr;
                }
            } else {
                merged_msgs.push_back(m);
            }
        }
        if (merged_msgs.empty()) {
            merged_msgs.push_back({{"role", "user"}, {"content", "..."}});
        }
        anthropic["messages"] = merged_msgs;

        if (req.contains("tools") && req["tools"].is_array() && !req["tools"].empty()) {
            json tools = json::array();
            for (const auto &t : req["tools"]) {
                if (t.value("type", "") == "function" && t.contains("function")) {
                    const auto &fn = t["function"];
                    tools.push_back({
                        {"name", fn.value("name", "")},
                        {"description", fn.value("description", "")},
                        {"input_schema", fn.value("parameters", json::object())}
                    });
                }
            }
            if (!tools.empty()) {
                anthropic["tools"] = tools;
            }
        }
        return dumpJson(anthropic);
    }

    if (shape == WireShape::Gemini) {
        json gemini = json::object();
        json contents = json::array();
        json system_inst;

        if (req.contains("messages") && req["messages"].is_array()) {
            for (const auto &m : req["messages"]) {
                if (!m.is_object()) continue;
                std::string role = m.value("role", "user");
                if (role == "system") {
                    if (m.contains("content") && m["content"].is_string()) {
                        system_inst = {{"parts", {{{"text", m["content"].get<std::string>()}}}}};
                    }
                } else {
                    std::string gemini_role = (role == "assistant") ? "model" : "user";
                    json parts = json::array();
                    if (m.contains("content")) {
                        if (m["content"].is_string()) {
                            parts.push_back({{"text", m["content"].get<std::string>()}});
                        } else if (m["content"].is_array()) {
                            for (const auto &p : m["content"]) {
                                if (p.value("type", "") == "text" && p.contains("text")) {
                                    parts.push_back({{"text", p["text"].get<std::string>()}});
                                }
                            }
                        }
                    }
                    if (!parts.empty()) {
                        contents.push_back({{"role", gemini_role}, {"parts", parts}});
                    }
                }
            }
        }
        if (contents.empty()) {
            contents.push_back({{"role", "user"}, {"parts", {{{"text", "..."}}}}});
        }
        gemini["contents"] = contents;
        if (!system_inst.empty()) {
            gemini["systemInstruction"] = system_inst;
        }

        json gen_config = json::object();
        if (req.contains("temperature")) gen_config["temperature"] = req["temperature"];
        if (req.contains("top_p")) gen_config["topP"] = req["top_p"];
        if (req.contains("max_tokens")) gen_config["maxOutputTokens"] = req["max_tokens"];
        else if (req.contains("max_completion_tokens")) gen_config["maxOutputTokens"] = req["max_completion_tokens"];
        if (!gen_config.empty()) {
            gemini["generationConfig"] = gen_config;
        }
        return dumpJson(gemini);
    }

    if (shape == WireShape::Responses) {
        return dumpJson(chatRequestToResponse(req, upstream_model, stream));
    }

    if (shape == WireShape::Ollama) {
        // Ollama's /api/chat is OpenAI-shaped at the message level but puts its
        // sampling knobs under `options` with its own names (num_predict for
        // max_tokens), and it has no `developer` role.
        json out = json::object();
        out["model"] = std::string{upstream_model};
        out["stream"] = stream;
        json messages = json::array();
        if (req.contains("messages") && req["messages"].is_array()) {
            for (const auto &m : req["messages"]) {
                if (!m.is_object()) continue;
                json msg = json::object();
                const std::string role = m.value("role", "user");
                msg["role"] = role == "developer" ? "system" : role;
                msg["content"] = contentAsText(m);
                if (m.contains("tool_calls")) msg["tool_calls"] = m["tool_calls"];
                if (m.contains("tool_call_id")) msg["tool_call_id"] = m["tool_call_id"];
                // Ollama takes images as bare base64 strings, not as data URLs.
                if (m.contains("content") && m["content"].is_array()) {
                    json images = json::array();
                    for (const auto &part : m["content"]) {
                        if (!part.is_object() || part.value("type", "") != "image_url" ||
                            !part.contains("image_url")) {
                            continue;
                        }
                        std::string mime;
                        std::string base64;
                        if (splitDataUrl(part["image_url"].value("url", ""), mime, base64)) {
                            images.push_back(std::move(base64));
                        }
                    }
                    if (!images.empty()) msg["images"] = std::move(images);
                }
                messages.push_back(std::move(msg));
            }
        }
        out["messages"] = std::move(messages);
        json options = json::object();
        if (req.contains("temperature")) options["temperature"] = req["temperature"];
        if (req.contains("top_p")) options["top_p"] = req["top_p"];
        if (req.contains("max_tokens")) options["num_predict"] = req["max_tokens"];
        else if (req.contains("max_completion_tokens")) options["num_predict"] = req["max_completion_tokens"];
        if (req.contains("stop")) options["stop"] = req["stop"];
        if (req.contains("frequency_penalty")) options["frequency_penalty"] = req["frequency_penalty"];
        if (req.contains("presence_penalty")) options["presence_penalty"] = req["presence_penalty"];
        if (!options.empty()) out["options"] = std::move(options);
        // Ollama's tool schema is already OpenAI's, so this is a pass-through
        // rather than a translation.
        if (req.contains("tools")) out["tools"] = req["tools"];
        return dumpJson(out);
    }

    if (shape == WireShape::Bedrock) {
        // The Converse API. Its shape is its own: content is a list of typed
        // blocks (text / image / toolUse / toolResult), the system prompt is a
        // separate top-level list rather than a message, and every sampling knob
        // lives under inferenceConfig.
        json out = json::object();
        json messages = json::array();
        json system_blocks = json::array();
        if (req.contains("messages") && req["messages"].is_array()) {
            for (const auto &m : req["messages"]) {
                if (!m.is_object()) continue;
                const std::string role = m.value("role", "user");
                if (role == "system" || role == "developer") {
                    const std::string text = contentAsText(m);
                    if (!text.empty()) system_blocks.push_back({{"text", text}});
                    continue;
                }
                json msg = json::object();
                json blocks = json::array();
                if (role == "tool" || role == "function") {
                    // Converse has no tool role: a result is a user turn holding
                    // a toolResult block.
                    msg["role"] = "user";
                    json result = json::object();
                    result["toolUseId"] = m.value("tool_call_id", m.value("name", "call_0"));
                    result["content"] = json::array({json{{"text", contentAsText(m)}}});
                    blocks.push_back({{"toolResult", result}});
                } else if (role == "assistant" && m.contains("tool_calls") &&
                           m["tool_calls"].is_array() && !m["tool_calls"].empty()) {
                    msg["role"] = "assistant";
                    const std::string text = contentAsText(m);
                    if (!text.empty()) blocks.push_back({{"text", text}});
                    for (const auto &tc : m["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        json use = json::object();
                        use["toolUseId"] = tc.value("id", "call_" + hexId(8));
                        if (tc.contains("function") && tc["function"].is_object()) {
                            use["name"] = tc["function"].value("name", "");
                            const json parsed = json::parse(
                                tc["function"].value("arguments", "{}"), nullptr, false);
                            use["input"] = parsed.is_discarded() ? json::object() : parsed;
                        }
                        blocks.push_back({{"toolUse", use}});
                    }
                } else {
                    msg["role"] = role == "assistant" ? "assistant" : "user";
                    const std::string text = contentAsText(m);
                    if (!text.empty()) blocks.push_back({{"text", text}});
                    if (m.contains("content") && m["content"].is_array()) {
                        for (const auto &part : m["content"]) {
                            if (!part.is_object() || part.value("type", "") != "image_url" ||
                                !part.contains("image_url")) {
                                continue;
                            }
                            std::string mime;
                            std::string base64;
                            if (!splitDataUrl(part["image_url"].value("url", ""), mime, base64)) {
                                continue;
                            }
                            const std::string format = mime == "image/png"    ? "png"
                                                       : mime == "image/gif"  ? "gif"
                                                       : mime == "image/webp" ? "webp"
                                                                               : "jpeg";
                            blocks.push_back({{"image",
                                               {{"format", format},
                                                {"source", {{"bytes", std::move(base64)}}}}}});
                        }
                    }
                }
                if (blocks.empty()) blocks.push_back({{"text", ""}});
                msg["content"] = std::move(blocks);
                messages.push_back(std::move(msg));
            }
        }
        if (messages.empty()) {
            messages.push_back({{"role", "user"}, {"content", json::array({json{{"text", "..."}}})}});
        }
        if (!system_blocks.empty()) out["system"] = std::move(system_blocks);
        out["messages"] = std::move(messages);
        json config = json::object();
        if (req.contains("max_tokens")) config["maxTokens"] = req["max_tokens"];
        else if (req.contains("max_completion_tokens")) config["maxTokens"] = req["max_completion_tokens"];
        if (req.contains("temperature")) config["temperature"] = req["temperature"];
        if (req.contains("top_p")) config["topP"] = req["top_p"];
        if (req.contains("stop")) {
            config["stopSequences"] =
                req["stop"].is_string() ? json::array({req["stop"]}) : req["stop"];
        }
        if (!config.empty()) out["inferenceConfig"] = std::move(config);
        if (req.contains("tools") && req["tools"].is_array() && !req["tools"].empty()) {
            json tools = json::array();
            for (const auto &t : req["tools"]) {
                if (!t.is_object() || t.value("type", "") != "function" || !t.contains("function")) {
                    continue;
                }
                const auto &fn = t["function"];
                tools.push_back({{"toolSpec",
                                  {{"name", fn.value("name", "")},
                                   {"description", fn.value("description", "")},
                                   {"inputSchema",
                                    {{"json", fn.value("parameters", json::object())}}}}}});
            }
            if (!tools.empty()) out["toolConfig"] = {{"tools", std::move(tools)}};
        }
        return dumpJson(out);
    }

    // Default OpenAI format
    if (req.value("model", "") == upstream_model &&
        ((!req.contains("stream") && !stream) || (req.contains("stream") && req["stream"].get<bool>() == stream))) {
        return std::string{openai_request_json};
    }
    json out = req;
    out["model"] = std::string{upstream_model};
    if (stream || req.contains("stream")) {
        out["stream"] = stream;
    }
    return dumpJson(out);
}

std::string adaptChatResponse(const ProviderConfig &provider,
                              std::string_view upstream_response,
                              std::string_view requested_model) {
    const WireShape shape = wireShapeOf(provider.protocol);
    if (shape == WireShape::OpenAi) {
        // OpenAI's own shape needs no conversion at all. A Responses-shaped
        // upstream body does: it is a different document (output/usage instead
        // of choices/usage) that still has to reach a chat client as a chat
        // completion.
        return std::string{upstream_response};
    }

    const json root = json::parse(upstream_response, nullptr, false);
    if (root.is_discarded() || !root.is_object() || (root.contains("error") && !root["error"].is_null())) {
        return std::string{upstream_response};
    }

    if (shape == WireShape::Ollama) {
        json choice = json::object();
        choice["index"] = 0;
        json message = json::object();
        message["role"] = "assistant";
        std::string content;
        std::string reasoning;
        json tool_calls = json::array();
        if (root.contains("message") && root["message"].is_object()) {
            const auto &m = root["message"];
            content = m.value("content", "");
            // Ollama names reasoning `thinking`.
            reasoning = m.value("thinking", "");
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto &tc : m["tool_calls"]) {
                    if (!tc.is_object() || !tc.contains("function")) continue;
                    const auto &fn = tc["function"];
                    json call = json::object();
                    call["id"] = "call_" + hexId(8);
                    call["type"] = "function";
                    call["function"] = json::object();
                    call["function"]["name"] = fn.value("name", "");
                    call["function"]["arguments"] =
                        fn.contains("arguments") && fn["arguments"].is_string()
                            ? fn["arguments"].get<std::string>()
                            : dumpJson(fn.value("arguments", json::object()));
                    tool_calls.push_back(std::move(call));
                }
            }
        }
        message["content"] = content;
        if (!reasoning.empty()) message["reasoning_content"] = reasoning;
        if (!tool_calls.empty()) message["tool_calls"] = tool_calls;
        choice["message"] = std::move(message);
        if (!tool_calls.empty()) {
            choice["finish_reason"] = "tool_calls";
        } else if (root.value("done_reason", "") == "length") {
            choice["finish_reason"] = "length";
        } else {
            choice["finish_reason"] = "stop";
        }
        const auto prompt = root.value("prompt_eval_count", std::uint64_t{0});
        const auto completion = root.value("eval_count", std::uint64_t{0});
        json out = json::object();
        out["id"] = "chatcmpl-" + hexId(8);
        out["object"] = "chat.completion";
        out["created"] = static_cast<long long>(nowUnix());
        out["model"] = std::string{requested_model};
        out["choices"] = json::array({std::move(choice)});
        out["usage"] = json::object();
        out["usage"]["prompt_tokens"] = prompt;
        out["usage"]["completion_tokens"] = completion;
        out["usage"]["total_tokens"] = prompt + completion;
        return dumpJson(out);
    }

    if (shape == WireShape::Bedrock) {
        json choice = json::object();
        choice["index"] = 0;
        json message = json::object();
        message["role"] = "assistant";
        std::string content;
        std::string reasoning;
        json tool_calls = json::array();
        if (root.contains("output") && root["output"].is_object() &&
            root["output"].contains("message") && root["output"]["message"].is_object()) {
            const auto &m = root["output"]["message"];
            if (m.contains("content") && m["content"].is_array()) {
                for (const auto &block : m["content"]) {
                    if (!block.is_object()) continue;
                    if (block.contains("text")) {
                        content += block.value("text", "");
                    } else if (block.contains("reasoningContent") &&
                               block["reasoningContent"].is_object()) {
                        const auto &r = block["reasoningContent"];
                        if (r.contains("reasoningText") && r["reasoningText"].is_object()) {
                            reasoning += r["reasoningText"].value("text", "");
                        }
                    } else if (block.contains("toolUse") && block["toolUse"].is_object()) {
                        const auto &use = block["toolUse"];
                        json call = json::object();
                        call["id"] = use.value("toolUseId", "call_" + hexId(8));
                        call["type"] = "function";
                        call["function"] = json::object();
                        call["function"]["name"] = use.value("name", "");
                        call["function"]["arguments"] =
                            use.contains("input") ? dumpJson(use["input"]) : std::string{"{}"};
                        tool_calls.push_back(std::move(call));
                    }
                }
            }
        }
        message["content"] = content;
        if (!reasoning.empty()) message["reasoning_content"] = reasoning;
        if (!tool_calls.empty()) message["tool_calls"] = tool_calls;
        choice["message"] = std::move(message);
        const std::string stop = root.value("stopReason", "");
        if (!tool_calls.empty()) {
            choice["finish_reason"] = "tool_calls";
        } else if (stop == "max_tokens") {
            choice["finish_reason"] = "length";
        } else {
            choice["finish_reason"] = "stop";
        }
        std::uint64_t prompt = 0;
        std::uint64_t completion = 0;
        if (root.contains("usage") && root["usage"].is_object()) {
            prompt = root["usage"].value("inputTokens", std::uint64_t{0});
            completion = root["usage"].value("outputTokens", std::uint64_t{0});
        }
        json out = json::object();
        out["id"] = "chatcmpl-" + hexId(8);
        out["object"] = "chat.completion";
        out["created"] = static_cast<long long>(nowUnix());
        out["model"] = std::string{requested_model};
        out["choices"] = json::array({std::move(choice)});
        out["usage"] = json::object();
        out["usage"]["prompt_tokens"] = prompt;
        out["usage"]["completion_tokens"] = completion;
        out["usage"]["total_tokens"] = prompt + completion;
        return dumpJson(out);
    }

    if (shape == WireShape::Anthropic) {
        json choice = json::object();
        choice["index"] = 0;
        json message = json::object();
        message["role"] = "assistant";
        std::string text_content;
        std::string reasoning_content;
        json tool_calls = json::array();

        if (root.contains("content") && root["content"].is_array()) {
            for (const auto &block : root["content"]) {
                std::string btype = block.value("type", "");
                if (btype == "text" && block.contains("text")) {
                    text_content += block["text"].get<std::string>();
                } else if (btype == "thinking" && block.contains("thinking")) {
                    // Extended thinking: the reasoning the client asked for and
                    // paid for, which had nowhere to go before this. It leaves as
                    // `reasoning_content`, the field OpenAI-compatible clients and
                    // relays read. `redacted_thinking` is left alone — its content
                    // is encrypted and there is nothing readable to hand on.
                    reasoning_content += block["thinking"].get<std::string>();
                } else if (btype == "tool_use") {
                    json tc = json::object();
                    tc["id"] = block.value("id", "call_" + hexId(8));
                    tc["type"] = "function";
                    json fn = json::object();
                    fn["name"] = block.value("name", "");
                    fn["arguments"] = block.contains("input") ? dumpJson(block["input"]) : "{}";
                    tc["function"] = fn;
                    tool_calls.push_back(tc);
                }
            }
        }
        message["content"] = text_content;
        if (!reasoning_content.empty()) {
            message["reasoning_content"] = reasoning_content;
        }
        if (!tool_calls.empty()) {
            message["tool_calls"] = tool_calls;
        }
        choice["message"] = message;

        std::string stop_reason = root.value("stop_reason", "end_turn");
        std::string finish_reason = "stop";
        if (stop_reason == "max_tokens") finish_reason = "length";
        else if (stop_reason == "tool_use") finish_reason = "tool_calls";
        choice["finish_reason"] = finish_reason;

        json out = json::object();
        out["id"] = "chatcmpl-" + root.value("id", hexId(12));
        out["object"] = "chat.completion";
        out["created"] = static_cast<long long>(nowUnix());
        out["model"] = std::string{requested_model};
        out["choices"] = json::array({choice});

        int in_tokens = 0;
        int out_tokens = 0;
        if (root.contains("usage") && root["usage"].is_object()) {
            in_tokens = root["usage"].value("input_tokens", 0);
            out_tokens = root["usage"].value("output_tokens", 0);
        }
        out["usage"] = {
            {"prompt_tokens", in_tokens},
            {"completion_tokens", out_tokens},
            {"total_tokens", in_tokens + out_tokens}
        };
        return dumpJson(out);
    }

    if (shape == WireShape::Gemini) {
        json choice = json::object();
        choice["index"] = 0;
        json message = json::object();
        message["role"] = "assistant";
        std::string text_content;
        std::string reasoning_content;

        if (root.contains("candidates") && root["candidates"].is_array() && !root["candidates"].empty()) {
            const auto &cand = root["candidates"][0];
            if (cand.contains("content") && cand["content"].contains("parts")) {
                for (const auto &p : cand["content"]["parts"]) {
                    if (!p.contains("text")) {
                        continue;
                    }
                    // `thought` marks the model's reasoning. Appending it to the
                    // content — which is what this loop used to do — handed the
                    // caller the model's thinking as if it were the answer.
                    if (p.value("thought", false)) {
                        reasoning_content += p["text"].get<std::string>();
                    } else {
                        text_content += p["text"].get<std::string>();
                    }
                }
            }
            std::string finish = cand.value("finishReason", "STOP");
            if (finish == "STOP") choice["finish_reason"] = "stop";
            else if (finish == "MAX_TOKENS") choice["finish_reason"] = "length";
            else choice["finish_reason"] = "stop";
        } else {
            choice["finish_reason"] = "stop";
        }
        message["content"] = text_content;
        if (!reasoning_content.empty()) {
            message["reasoning_content"] = reasoning_content;
        }
        choice["message"] = message;

        json out = json::object();
        out["id"] = "chatcmpl-gemini-" + hexId(12);
        out["object"] = "chat.completion";
        out["created"] = static_cast<long long>(nowUnix());
        out["model"] = std::string{requested_model};
        out["choices"] = json::array({choice});

        int prompt_tokens = 0;
        int cand_tokens = 0;
        if (root.contains("usageMetadata") && root["usageMetadata"].is_object()) {
            prompt_tokens = root["usageMetadata"].value("promptTokenCount", 0);
            cand_tokens = root["usageMetadata"].value("candidatesTokenCount", 0);
        }
        out["usage"] = {
            {"prompt_tokens", prompt_tokens},
            {"completion_tokens", cand_tokens},
            {"total_tokens", prompt_tokens + cand_tokens}
        };
        return dumpJson(out);
    }

    if (shape == WireShape::Responses) {
        json message = {{"role", "assistant"}};
        std::string text_content;
        std::string reasoning_content;
        std::string refusal;
        json calls = json::array();
        if (root.contains("output") && root["output"].is_array()) {
            for (const auto &item : root["output"]) {
                if (!item.is_object()) continue;
                const std::string type = item.value("type", "");
                if (type == "reasoning") {
                    // A reasoning item carries its readable text as a summary;
                    // some relays use content. Encrypted reasoning has no Chat
                    // equivalent and must not become ordinary assistant text.
                    for (const char *field : {"summary", "content"}) {
                        if (!item.contains(field) || !item[field].is_array()) continue;
                        for (const auto &part : item[field]) {
                            if (part.is_object()) reasoning_content += part.value("text", "");
                        }
                    }
                } else if (type == "function_call") {
                    calls.push_back(responseFunctionToChat(item));
                } else if (type == "message" && item.contains("content") && item["content"].is_array()) {
                    for (const auto &part : item["content"]) {
                        if (!part.is_object()) continue;
                        const std::string part_type = part.value("type", "");
                        // Retain the historical relay alias while accepting the
                        // actual Responses API output content discriminator.
                        if (part_type == "output_text" || part_type == "text") text_content += part.value("text", "");
                        else if (part_type == "refusal") refusal += part.value("refusal", "");
                    }
                }
            }
        }
        message["content"] = text_content.empty() && !calls.empty() ? json(nullptr) : json(text_content);
        if (!reasoning_content.empty()) message["reasoning_content"] = reasoning_content;
        if (!refusal.empty()) message["refusal"] = refusal;
        if (!calls.empty()) message["tool_calls"] = calls;
        std::string finish = calls.empty() ? "stop" : "tool_calls";
        if (root.value("status", "") == "incomplete") {
            const auto details = root.value("incomplete_details", json::object());
            finish = details.is_object() && details.value("reason", "") == "content_filter" ? "content_filter" : "length";
        }
        json out = {{"id", "chatcmpl-" + root.value("id", hexId(12))},
                    {"object", "chat.completion"},
                    {"created", root.value("created_at", static_cast<long long>(nowUnix()))},
                    {"model", std::string{requested_model}},
                    {"choices", json::array({{{"index", 0}, {"message", std::move(message)}, {"finish_reason", finish}}})}};
        if (root.contains("usage") && root["usage"].is_object()) out["usage"] = responseUsageToChat(root["usage"]);
        return dumpJson(out);
    }

    return std::string{upstream_response};
}

std::string adaptResponsesToChat(std::string_view responses_request_json) {
    const json req = json::parse(responses_request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return std::string{responses_request_json};
    }

    json chat = {{"model", req.value("model", "")}, {"stream", req.value("stream", false)}};
    json messages = json::array();
    if (req.contains("instructions") && req["instructions"].is_string() && !req["instructions"].empty()) {
        messages.push_back({{"role", "system"}, {"content", req["instructions"]}});
    }
    if (req.contains("input")) {
        if (req["input"].is_string()) {
            messages.push_back({{"role", "user"}, {"content", req["input"]}});
        } else if (req["input"].is_array()) {
            for (const auto &item : req["input"]) {
                if (!item.is_object()) continue;
                const std::string type = item.value("type", "message");
                if (type == "function_call") {
                    // Parallel calls belong to the same assistant turn. A tool
                    // result following it can then reference each id exactly.
                    if (messages.empty() || messages.back().value("role", "") != "assistant") {
                        messages.push_back({{"role", "assistant"}, {"content", nullptr}});
                    }
                    auto &message = messages.back();
                    if (!message.contains("tool_calls")) message["tool_calls"] = json::array();
                    message["tool_calls"].push_back(responseFunctionToChat(item));
                } else if (type == "function_call_output") {
                    json output = item.value("output", json(""));
                    if (output.is_array()) output = responseContentToChat(output);
                    messages.push_back({{"role", "tool"}, {"tool_call_id", item.value("call_id", "")},
                                        {"content", std::move(output)}});
                } else if (type == "message" && item.contains("content")) {
                    const std::string role = item.value("role", "user");
                    json content = responseContentToChat(item["content"]);
                    if (role == "assistant" && !messages.empty() && messages.back().value("role", "") == "assistant") {
                        // Responses may place a message between function_call
                        // items and their results. They still form one assistant
                        // turn: Chat rejects another assistant message between
                        // its tool_calls and the corresponding tool messages.
                        auto &previous = messages.back()["content"];
                        if (previous.is_null()) previous = std::move(content);
                        else if (previous.is_string() && content.is_string()) previous = previous.get<std::string>() + content.get<std::string>();
                        else {
                            if (previous.is_string()) previous = json::array({{{"type", "text"}, {"text", previous}}});
                            if (content.is_string()) content = json::array({{{"type", "text"}, {"text", content}}});
                            if (previous.is_array() && content.is_array()) {
                                for (const auto &part : content) previous.push_back(part);
                            }
                        }
                    } else messages.push_back({{"role", role}, {"content", std::move(content)}});
                }
            }
        }
    }
    chat["messages"] = std::move(messages);
    copyResponseOptions(req, chat);
    if (req.contains("max_output_tokens")) chat["max_completion_tokens"] = req["max_output_tokens"];
    if (req.contains("reasoning") && req["reasoning"].is_object() && req["reasoning"].contains("effort")) {
        chat["reasoning_effort"] = req["reasoning"]["effort"];
    }
    if (req.contains("tools") && req["tools"].is_array()) {
        json tools = json::array();
        for (const auto &tool : req["tools"]) {
            if (!tool.is_object() || tool.value("type", "") != "function") continue;
            json fn = tool;
            fn.erase("type");
            tools.push_back({{"type", "function"}, {"function", std::move(fn)}});
        }
        chat["tools"] = std::move(tools);
    }
    if (req.contains("tool_choice")) {
        const auto &choice = req["tool_choice"];
        if (choice.is_object() && choice.value("type", "") == "function") {
            chat["tool_choice"] = {{"type", "function"}, {"function", {{"name", choice.value("name", "")}}}};
        } else if (choice.is_string()) chat["tool_choice"] = choice;
    }
    if (req.contains("text") && req["text"].is_object() && req["text"].contains("format") &&
        req["text"]["format"].is_object()) {
        const auto &format = req["text"]["format"];
        if (format.value("type", "") == "json_schema") {
            json schema = format;
            schema.erase("type");
            chat["response_format"] = {{"type", "json_schema"}, {"json_schema", std::move(schema)}};
        } else chat["response_format"] = format;
    }
    return dumpJson(chat);
}

std::string adaptChatToResponses(std::string_view chat_completion_response_json,
                                 std::string_view requested_model) {
    const json root = json::parse(chat_completion_response_json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || (root.contains("error") && !root["error"].is_null())) {
        return std::string{chat_completion_response_json};
    }

    json output = json::array();
    std::string finish = "stop";
    if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
        const auto &choice = root["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) finish = choice["finish_reason"].get<std::string>();
        if (choice.contains("message") && choice["message"].is_object()) {
            const auto &message = choice["message"];
            // Reasoning first: it is a distinct output item, not answer text.
            std::string reasoning = message.value("reasoning_content", "");
            if (reasoning.empty()) reasoning = message.value("reasoning", "");
            if (!reasoning.empty()) {
                output.push_back({{"id", "rs_" + hexId(12)}, {"type", "reasoning"},
                                  {"summary", json::array({{{"type", "summary_text"}, {"text", reasoning}}})}});
            }
            json content = json::array();
            const std::string text = contentAsText(message);
            if (!text.empty()) content.push_back({{"type", "output_text"}, {"text", text}, {"annotations", json::array()}});
            if (message.contains("refusal") && message["refusal"].is_string() && !message["refusal"].empty()) {
                content.push_back({{"type", "refusal"}, {"refusal", message["refusal"]}});
            }
            const bool has_calls = message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty();
            if (!content.empty() || !has_calls) {
                output.push_back({{"id", "msg_" + hexId(12)}, {"type", "message"}, {"role", "assistant"},
                                  {"status", finish == "length" || finish == "content_filter" ? "incomplete" : "completed"},
                                  {"content", std::move(content)}});
            }
            if (has_calls) {
                for (const auto &call : message["tool_calls"]) {
                    if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) continue;
                    const auto &fn = call["function"];
                    output.push_back({{"id", "fc_" + hexId(12)}, {"type", "function_call"}, {"status", "completed"},
                                      {"call_id", call.value("id", "call_" + hexId(8))},
                                      {"name", fn.value("name", "")}, {"arguments", fn.value("arguments", "")}});
                }
            }
        }
    }
    const bool incomplete = finish == "length" || finish == "content_filter";
    json resp = {{"id", "resp_" + root.value("id", hexId(12))}, {"object", "response"},
                 {"created_at", root.value("created", static_cast<long long>(nowUnix()))},
                 {"status", incomplete ? "incomplete" : "completed"}, {"model", std::string{requested_model}},
                 {"output", std::move(output)}, {"error", nullptr}, {"incomplete_details", nullptr}};
    if (incomplete) resp["incomplete_details"] = {{"reason", finish == "length" ? "max_output_tokens" : "content_filter"}};
    if (root.contains("usage") && root["usage"].is_object()) resp["usage"] = chatUsageToResponse(root["usage"]);
    return dumpJson(resp);
}

std::string adaptAnthropicToChat(std::string_view anthropic_request_json) {
    const json req = json::parse(anthropic_request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return std::string{anthropic_request_json};
    }

    json chat = json::object();
    chat["model"] = req.value("model", "");
    chat["stream"] = req.value("stream", false);

    if (req.contains("temperature")) chat["temperature"] = req["temperature"];
    if (req.contains("top_p")) chat["top_p"] = req["top_p"];
    if (req.contains("max_tokens")) chat["max_tokens"] = req["max_tokens"];

    json msgs = json::array();

    if (req.contains("system")) {
        std::string sys_text;
        if (req["system"].is_string()) {
            sys_text = req["system"].get<std::string>();
        } else if (req["system"].is_array()) {
            for (const auto &b : req["system"]) {
                if (b.is_object() && b.value("type", "") == "text" && b.contains("text")) {
                    if (!sys_text.empty()) sys_text += "\n\n";
                    sys_text += b["text"].get<std::string>();
                }
            }
        }
        if (!sys_text.empty()) {
            msgs.push_back({{"role", "system"}, {"content", sys_text}});
        }
    }

    if (req.contains("messages") && req["messages"].is_array()) {
        for (const auto &m : req["messages"]) {
            if (!m.is_object()) continue;
            std::string role = m.value("role", "user");

            if (m.contains("content")) {
                if (m["content"].is_string()) {
                    msgs.push_back({{"role", role}, {"content", m["content"].get<std::string>()}});
                } else if (m["content"].is_array()) {
                    if (role == "assistant") {
                        std::string text_acc;
                        std::string reasoning_acc;
                        json tool_calls = json::array();
                        for (const auto &block : m["content"]) {
                            if (!block.is_object()) continue;
                            std::string btype = block.value("type", "");
                            if (btype == "text" && block.contains("text")) {
                                text_acc += block["text"].get<std::string>();
                            } else if (btype == "thinking" && block.contains("thinking")) {
                                // Carried across so a reasoning-capable upstream
                                // sees the conversation the client believes it is
                                // having; an upstream that does not know the field
                                // ignores it.
                                reasoning_acc += block["thinking"].get<std::string>();
                            } else if (btype == "tool_use") {
                                json tc = json::object();
                                tc["id"] = block.value("id", "call_" + hexId(8));
                                tc["type"] = "function";
                                json fn = json::object();
                                fn["name"] = block.value("name", "");
                                fn["arguments"] =
                                    block.contains("input")
                                        ? (block["input"].is_string()
                                               ? block["input"].get<std::string>()
                                               : dumpJson(block["input"]))
                                        : "{}";
                                tc["function"] = fn;
                                tool_calls.push_back(tc);
                            }
                        }
                        json asst_msg = {{"role", "assistant"}, {"content", text_acc}};
                        if (!reasoning_acc.empty()) {
                            asst_msg["reasoning_content"] = reasoning_acc;
                        }
                        if (!tool_calls.empty()) {
                            asst_msg["tool_calls"] = tool_calls;
                        }
                        msgs.push_back(asst_msg);
                    } else { // user role
                        json user_content_parts = json::array();
                        bool has_complex = false;
                        for (const auto &block : m["content"]) {
                            if (!block.is_object()) continue;
                            std::string btype = block.value("type", "");
                            if (btype == "tool_result") {
                                if (!user_content_parts.empty()) {
                                    msgs.push_back({{"role", "user"}, {"content", user_content_parts}});
                                    user_content_parts.clear();
                                }
                                json tool_msg = json::object();
                                tool_msg["role"] = "tool";
                                tool_msg["tool_call_id"] = block.value("tool_use_id", "");
                                if (block.contains("content")) {
                                    tool_msg["content"] = block["content"].is_string()
                                        ? block["content"].get<std::string>()
                                        : dumpJson(block["content"]);
                                } else {
                                    tool_msg["content"] = "";
                                }
                                msgs.push_back(tool_msg);
                            } else if (btype == "text") {
                                user_content_parts.push_back({{"type", "text"}, {"text", block.value("text", "")}});
                            } else if (btype == "image") {
                                if (block.contains("source") && block["source"].value("type", "") == "base64") {
                                    std::string mime = block["source"].value("media_type", "image/png");
                                    std::string b64 = block["source"].value("data", "");
                                    user_content_parts.push_back({
                                        {"type", "image_url"},
                                        {"image_url", {{"url", "data:" + mime + ";base64," + b64}}}
                                    });
                                    has_complex = true;
                                }
                            }
                        }
                        if (!user_content_parts.empty()) {
                            if (!has_complex && user_content_parts.size() == 1 && user_content_parts[0].value("type", "") == "text") {
                                msgs.push_back({{"role", "user"}, {"content", user_content_parts[0]["text"]}});
                            } else {
                                msgs.push_back({{"role", "user"}, {"content", user_content_parts}});
                            }
                        }
                    }
                }
            }
        }
    }

    chat["messages"] = msgs;

    if (req.contains("tools") && req["tools"].is_array() && !req["tools"].empty()) {
        json tools = json::array();
        for (const auto &t : req["tools"]) {
            if (!t.is_object()) continue;
            tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", t.value("name", "")},
                    {"description", t.value("description", "")},
                    {"parameters", t.value("input_schema", json::object())}
                }}
            });
        }
        if (!tools.empty()) {
            chat["tools"] = tools;
        }
    }

    return dumpJson(chat);
}

std::string adaptGeminiToChat(std::string_view gemini_request_json, std::string_view model) {
    const json req = json::parse(gemini_request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return std::string{gemini_request_json};
    }

    json chat = json::object();
    chat["model"] = std::string{model};

    json msgs = json::array();

    if (req.contains("systemInstruction") && req["systemInstruction"].is_object()) {
        const auto &si = req["systemInstruction"];
        if (si.contains("parts") && si["parts"].is_array()) {
            std::string sys_text;
            for (const auto &p : si["parts"]) {
                if (p.is_object() && p.contains("text")) {
                    if (!sys_text.empty()) sys_text += "\n\n";
                    sys_text += p["text"].get<std::string>();
                }
            }
            if (!sys_text.empty()) {
                msgs.push_back({{"role", "system"}, {"content", sys_text}});
            }
        }
    }

    if (req.contains("contents") && req["contents"].is_array()) {
        for (const auto &c : req["contents"]) {
            if (!c.is_object()) continue;
            std::string role = c.value("role", "user");
            std::string chat_role = (role == "model") ? "assistant" : "user";
            std::string text_acc;
            json tool_calls = json::array();

            if (c.contains("parts") && c["parts"].is_array()) {
                for (const auto &p : c["parts"]) {
                    if (!p.is_object()) continue;
                    if (p.contains("text")) {
                        text_acc += p["text"].get<std::string>();
                    } else if (p.contains("functionCall")) {
                        const auto &fc = p["functionCall"];
                        json tc = json::object();
                        tc["id"] = "call_" + hexId(8);
                        tc["type"] = "function";
                        json fn = json::object();
                        fn["name"] = fc.value("name", "");
                        fn["arguments"] = fc.contains("args") ? dumpJson(fc["args"]) : "{}";
                        tc["function"] = fn;
                        tool_calls.push_back(tc);
                    } else if (p.contains("functionResponse")) {
                        const auto &fr = p["functionResponse"];
                        json tool_msg = json::object();
                        tool_msg["role"] = "tool";
                        tool_msg["tool_call_id"] = "call_0";
                        tool_msg["name"] = fr.value("name", "");
                        tool_msg["content"] =
                            fr.contains("response") ? dumpJson(fr["response"]) : "{}";
                        msgs.push_back(tool_msg);
                    }
                }
            }

            if (!text_acc.empty() || !tool_calls.empty()) {
                json msg = {{"role", chat_role}, {"content", text_acc}};
                if (!tool_calls.empty()) {
                    msg["tool_calls"] = tool_calls;
                }
                msgs.push_back(msg);
            }
        }
    }

    chat["messages"] = msgs;

    if (req.contains("generationConfig") && req["generationConfig"].is_object()) {
        const auto &gc = req["generationConfig"];
        if (gc.contains("temperature")) chat["temperature"] = gc["temperature"];
        if (gc.contains("topP")) chat["top_p"] = gc["topP"];
        if (gc.contains("maxOutputTokens")) chat["max_tokens"] = gc["maxOutputTokens"];
    }

    return dumpJson(chat);
}

std::string adaptChatToAnthropic(std::string_view chat_completion_response_json,
                                 std::string_view requested_model) {
    const json root = json::parse(chat_completion_response_json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.contains("error")) {
        return std::string{chat_completion_response_json};
    }

    json anthropic = json::object();
    anthropic["id"] = "msg_" + root.value("id", hexId(12));
    anthropic["type"] = "message";
    anthropic["role"] = "assistant";
    anthropic["model"] = std::string{requested_model};

    json content_blocks = json::array();
    std::string stop_reason = "end_turn";

    if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
        const auto &choice = root["choices"][0];
        if (choice.contains("message") && choice["message"].is_object()) {
            const auto &msg = choice["message"];
            // Reasoning first, the order Anthropic puts its blocks in. The block
            // carries no `signature`: that is issued by the provider that produced
            // the thinking, and a fabricated one is rejected outright — so a client
            // that echoes this turn back is handled by dropping it on the request
            // side rather than by inventing a signature here.
            std::string reasoning = msg.value("reasoning_content", "");
            if (reasoning.empty()) {
                reasoning = msg.value("reasoning", "");
            }
            if (!reasoning.empty()) {
                content_blocks.push_back({{"type", "thinking"}, {"thinking", reasoning}});
            }
            if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty()) {
                content_blocks.push_back({{"type", "text"}, {"text", msg["content"].get<std::string>()}});
            }
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto &tc : msg["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    json tu = json::object();
                    tu["type"] = "tool_use";
                    tu["id"] = tc.value("id", "call_" + hexId(8));
                    if (tc.contains("function") && tc["function"].is_object()) {
                        tu["name"] = tc["function"].value("name", "");
                        std::string args_str = tc["function"].value("arguments", "{}");
                        json parsed = json::parse(args_str, nullptr, false);
                        tu["input"] = parsed.is_discarded() ? json::object() : parsed;
                    }
                    content_blocks.push_back(tu);
                }
            }
        }
        std::string fr = choice.value("finish_reason", "stop");
        if (fr == "length") stop_reason = "max_tokens";
        else if (fr == "tool_calls") stop_reason = "tool_use";
        else stop_reason = "end_turn";
    }

    anthropic["content"] = content_blocks;
    anthropic["stop_reason"] = stop_reason;
    anthropic["stop_sequence"] = nullptr;

    int in_tokens = 0;
    int out_tokens = 0;
    if (root.contains("usage") && root["usage"].is_object()) {
        in_tokens = root["usage"].value("prompt_tokens", 0);
        out_tokens = root["usage"].value("completion_tokens", 0);
    }
    anthropic["usage"] = {
        {"input_tokens", in_tokens},
        {"output_tokens", out_tokens}
    };

    return dumpJson(anthropic);
}

std::string adaptChatToGemini(std::string_view chat_completion_response_json,
                              std::string_view requested_model) {
    const json root = json::parse(chat_completion_response_json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.contains("error")) {
        return std::string{chat_completion_response_json};
    }

    json gemini = json::object();
    json candidates = json::array();
    json cand = json::object();
    cand["index"] = 0;

    json parts = json::array();
    std::string finish_reason = "STOP";

    if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
        const auto &choice = root["choices"][0];
        if (choice.contains("message") && choice["message"].is_object()) {
            const auto &msg = choice["message"];
            // `thought: true` is how Gemini labels the model's reasoning, so a
            // Gemini-shaped client sees it where it expects to.
            const std::string reasoning = [&msg] {
                std::string text = msg.value("reasoning_content", "");
                if (text.empty()) {
                    text = msg.value("reasoning", "");
                }
                return text;
            }();
            if (!reasoning.empty()) {
                parts.push_back({{"text", reasoning}, {"thought", true}});
            }
            if (msg.contains("content") && msg["content"].is_string()) {
                parts.push_back({{"text", msg["content"].get<std::string>()}});
            }
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto &tc : msg["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    json fc = json::object();
                    if (tc.contains("function") && tc["function"].is_object()) {
                        fc["name"] = tc["function"].value("name", "");
                        std::string args_str = tc["function"].value("arguments", "{}");
                        json parsed = json::parse(args_str, nullptr, false);
                        fc["args"] = parsed.is_discarded() ? json::object() : parsed;
                    }
                    parts.push_back({{"functionCall", fc}});
                }
            }
        }
        std::string fr = choice.value("finish_reason", "stop");
        if (fr == "length") finish_reason = "MAX_TOKENS";
        else finish_reason = "STOP";
    }

    cand["content"] = {{"role", "model"}, {"parts", parts}};
    cand["finishReason"] = finish_reason;
    candidates.push_back(cand);
    gemini["candidates"] = candidates;

    int in_tokens = 0;
    int out_tokens = 0;
    if (root.contains("usage") && root["usage"].is_object()) {
        in_tokens = root["usage"].value("prompt_tokens", 0);
        out_tokens = root["usage"].value("completion_tokens", 0);
    }
    gemini["usageMetadata"] = {
        {"promptTokenCount", in_tokens},
        {"candidatesTokenCount", out_tokens},
        {"totalTokenCount", in_tokens + out_tokens}
    };
    gemini["modelVersion"] = std::string{requested_model};

    return dumpJson(gemini);
}

// ── StreamProtocolAdapter ───────────────────────────────────────────────────

class StreamProtocolAdapter::Impl {
public:
    Impl(std::string from_proto, std::string to_proto, std::string model, std::string request_id)
        : from_proto_(wireShapeName(wireShapeOf(from_proto))),
          to_proto_(wireShapeName(wireShapeOf(to_proto))),
          model_(std::move(model)), request_id_(std::move(request_id)) {
        if (from_proto_.empty()) from_proto_ = "openai";
        if (to_proto_.empty()) to_proto_ = "openai";
    }

    std::string feed(std::string_view chunk) {
        if (from_proto_ == to_proto_) {
            return std::string{chunk};
        }
        if (finished_ || bedrock_broken_) {
            return {};
        }

        buffer_.append(chunk);
        std::string out;

        for (;;) {
            if (finished_) break;
            std::string event_type;
            std::string data_str;

            if (from_proto_ == "ollama") {
                // Ollama streams newline-delimited JSON, not SSE: one complete
                // object per line, with no `data:` prefix and no blank-line
                // terminator. Reusing the SSE splitter here would buffer the
                // whole answer and emit nothing until the stream ended.
                const auto newline = buffer_.find('\n');
                if (newline == std::string::npos) break;
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (trim(line).empty()) continue;
                data_str = std::move(line);
            } else if (from_proto_ == "bedrock") {
                // AWS event stream: a length-prefixed binary protocol with CRC32
                // checks on both ends of every message.
                if (bedrock_broken_) break;
                if (!nextBedrockEvent(data_str, event_type)) break;
            } else {
                std::size_t delim_pos = std::string::npos;
                std::size_t delim_len = 0;
                auto pos_crlf = buffer_.find("\r\n\r\n");
                auto pos_lf = buffer_.find("\n\n");
                if (pos_crlf != std::string::npos && (pos_lf == std::string::npos || pos_crlf < pos_lf)) {
                    delim_pos = pos_crlf;
                    delim_len = 4;
                } else if (pos_lf != std::string::npos) {
                    delim_pos = pos_lf;
                    delim_len = 2;
                }
                if (delim_pos == std::string::npos) break;

                std::string event_block = buffer_.substr(0, delim_pos);
                buffer_.erase(0, delim_pos + delim_len);

                std::istringstream stream(event_block);
                std::string line;
                while (std::getline(stream, line)) {
                    if (line.ends_with('\r')) line.pop_back();
                    std::string_view line_sv = line;
                    if (startsWith(line_sv, "event:")) {
                        std::string_view v = line_sv.substr(6);
                        if (!v.empty() && v[0] == ' ') v.remove_prefix(1);
                        event_type = trim(v);
                    } else if (startsWith(line_sv, "data:")) {
                        std::string_view v = line_sv.substr(5);
                        if (!v.empty() && v[0] == ' ') v.remove_prefix(1);
                        if (!data_str.empty()) data_str += "\n";
                        data_str.append(v);
                    }
                }
            }

            if (data_str.empty()) continue;

            std::string text_delta;
            std::string reasoning_delta;
            std::string finish_reason;
            json tool_calls_delta = json::array();
            int out_tokens = 0;
            int in_tokens = 0;
            bool is_done = false;

            if (from_proto_ == "ollama") {
                // Every line is one object: `{"message":{"content":"..."},"done":false}`
                // per token, and a final `{"done":true,...}` carrying the counts.
                const json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    if (data.contains("message") && data["message"].is_object()) {
                        const auto &m = data["message"];
                        text_delta = m.value("content", "");
                        reasoning_delta = m.value("thinking", "");
                        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                            for (const auto &tc : m["tool_calls"]) {
                                if (!tc.is_object() || !tc.contains("function")) continue;
                                const auto &fn = tc["function"];
                                json call = json::object();
                                call["index"] = tool_index_++;
                                call["id"] = "call_" + hexId(8);
                                call["type"] = "function";
                                call["function"] = json::object();
                                call["function"]["name"] = fn.value("name", "");
                                call["function"]["arguments"] =
                                    fn.contains("arguments") && fn["arguments"].is_string()
                                        ? fn["arguments"].get<std::string>()
                                        : dumpJson(fn.value("arguments", json::object()));
                                tool_calls_delta.push_back(std::move(call));
                            }
                        }
                    }
                    if (data.value("done", false)) {
                        is_done = true;
                        finish_reason = data.value("done_reason", "") == "length" ? "length" : "stop";
                        if (!tool_calls_delta.empty()) finish_reason = "tool_calls";
                        in_tokens = static_cast<int>(data.value("prompt_eval_count", std::uint64_t{0}));
                        out_tokens = static_cast<int>(data.value("eval_count", std::uint64_t{0}));
                    }
                }
            } else if (from_proto_ == "bedrock") {
                // Converse stream events. The tool-use input arrives as a stream
                // of JSON *fragments* (not complete JSON), which is exactly what
                // OpenAI's tool_calls delta wants, so it is forwarded as-is.
                const json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    if (event_type == "contentBlockStart") {
                        if (data.contains("start") && data["start"].is_object() &&
                            data["start"].contains("toolUse")) {
                            const auto &use = data["start"]["toolUse"];
                            tool_id_ = use.value("toolUseId", "call_" + hexId(8));
                            tool_name_ = use.value("name", "");
                            json call = json::object();
                            call["index"] = tool_index_;
                            call["id"] = tool_id_;
                            call["type"] = "function";
                            call["function"] = json::object();
                            call["function"]["name"] = tool_name_;
                            call["function"]["arguments"] = "";
                            tool_calls_delta.push_back(std::move(call));
                        }
                    } else if (event_type == "contentBlockDelta" && data.contains("delta") &&
                               data["delta"].is_object()) {
                        const auto &delta = data["delta"];
                        if (delta.contains("text")) {
                            text_delta = delta.value("text", "");
                        } else if (delta.contains("reasoningContent") &&
                                   delta["reasoningContent"].is_object()) {
                            reasoning_delta = delta["reasoningContent"].value("text", "");
                        } else if (delta.contains("toolUse") && delta["toolUse"].is_object()) {
                            const std::string fragment = delta["toolUse"].value("input", "");
                            if (!fragment.empty()) {
                                json call = json::object();
                                call["index"] = tool_index_;
                                call["function"] = json::object();
                                call["function"]["arguments"] = fragment;
                                tool_calls_delta.push_back(std::move(call));
                            }
                        }
                    } else if (event_type == "contentBlockStop") {
                        ++tool_index_;
                    } else if (event_type == "messageStop") {
                        is_done = true;
                        const std::string stop = data.value("stopReason", "");
                        finish_reason = stop == "max_tokens" ? "length" : "stop";
                        if (stop == "tool_use") finish_reason = "tool_calls";
                    } else if (event_type == "metadata" && data.contains("usage") &&
                               data["usage"].is_object()) {
                        in_tokens = static_cast<int>(
                            data["usage"].value("inputTokens", std::uint64_t{0}));
                        out_tokens = static_cast<int>(
                            data["usage"].value("outputTokens", std::uint64_t{0}));
                    } else if (startsWith(event_type, "exception") ||
                               startsWith(event_type, "error")) {
                        is_done = true;
                        finish_reason = "stop";
                    }
                }
            } else if (from_proto_ == "openai_responses") {
                // Each output index identifies a Responses item; function call
                // ids identify the tool result in the next request. They are
                // different ids, and Chat must receive the latter.
                const json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    const std::string type = data.value("type", event_type);
                    if (type == "response.created" && data.contains("response") && data["response"].is_object()) {
                        stream_id_ = data["response"].value("id", request_id_);
                    } else if (type == "response.output_text.delta") {
                        text_delta = data.value("delta", "");
                    } else if (type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta") {
                        reasoning_delta = data.value("delta", "");
                    } else if ((type == "response.output_item.added" || type == "response.output_item.done") &&
                               data.contains("item") && data["item"].is_object() && data["item"].value("type", "") == "function_call") {
                        appendResponseCall(tool_calls_delta, data.value("output_index", 0), data["item"], true);
                    } else if (type == "response.function_call_arguments.delta" || type == "response.function_call_arguments.done") {
                        json item = {{"id", data.value("item_id", "")},
                                     {"arguments", data.value(type == "response.function_call_arguments.delta" ? "delta" : "arguments", "")}};
                        appendResponseCall(tool_calls_delta, data.value("output_index", 0), item,
                                           type == "response.function_call_arguments.done");
                    } else if (type == "response.completed" || type == "response.done" || type == "response.incomplete" || type == "response.failed") {
                        is_done = true;
                        if (data.contains("response") && data["response"].is_object()) {
                            const auto &response = data["response"];
                            if (response.contains("usage") && response["usage"].is_object()) {
                                response_usage_ = response["usage"];
                                in_tokens = response_usage_.value("input_tokens", 0);
                                out_tokens = response_usage_.value("output_tokens", 0);
                            }
                            // Some relays only populate tools in the final
                            // snapshot. Emit the missing suffix, never duplicate
                            // arguments already delivered in delta events.
                            if (response.contains("output") && response["output"].is_array()) {
                                int index = 0;
                                for (const auto &item : response["output"]) {
                                    if (item.is_object() && item.value("type", "") == "function_call") {
                                        appendResponseCall(tool_calls_delta, index, item, true);
                                    }
                                    ++index;
                                }
                            }
                            if (response.contains("error") && response["error"].is_object()) response_error_ = response["error"];
                            const auto details = response.value("incomplete_details", json::object());
                            if (type == "response.incomplete" || response.value("status", "") == "incomplete") {
                                finish_reason = details.is_object() && details.value("reason", "") == "content_filter" ? "content_filter" : "length";
                            }
                        }
                        if (type == "response.failed" && response_error_.is_null()) response_error_ = data;
                        if (finish_reason.empty()) finish_reason = incoming_response_calls_.empty() ? "stop" : "tool_calls";
                    } else if (type == "error") {
                        is_done = true;
                        response_error_ = data.contains("error") ? data["error"] : data;
                    }
                }
            } else if (from_proto_ == "openai") {
                if (trim(data_str) == "[DONE]") {
                    is_done = true;
                } else {
                    json data = json::parse(data_str, nullptr, false);
                    if (!data.is_discarded() && data.is_object()) {
                        if (data.contains("error") && data["error"].is_object()) {
                            response_error_ = data["error"];
                            is_done = true;
                        }
                        if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
                            const auto &choice = data["choices"][0];
                            if (choice.contains("delta") && choice["delta"].is_object()) {
                                if (choice["delta"].contains("content") && choice["delta"]["content"].is_string()) {
                                    text_delta = choice["delta"]["content"].get<std::string>();
                                }
                                if (choice["delta"].contains("tool_calls") && choice["delta"]["tool_calls"].is_array()) {
                                    tool_calls_delta = choice["delta"]["tool_calls"];
                                }
                                // Reasoning models name this field either way.
                                for (const char *field : {"reasoning_content", "reasoning"}) {
                                    if (choice["delta"].contains(field) &&
                                        choice["delta"][field].is_string()) {
                                        reasoning_delta += choice["delta"][field].get<std::string>();
                                    }
                                }
                            }
                            if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                                finish_reason = choice["finish_reason"].get<std::string>();
                                // Responses completion contains the full usage,
                                // which Chat may send in a separate trailing
                                // chunk before [DONE]. Keep reading that tail.
                                is_done = to_proto_ != "openai_responses";
                            }
                        }
                        if (data.contains("usage") && data["usage"].is_object()) {
                            in_tokens = data["usage"].value("prompt_tokens", 0);
                            out_tokens = data["usage"].value("completion_tokens", 0);
                            response_usage_ = chatUsageToResponse(data["usage"]);
                        }
                    }
                }
            } else if (from_proto_ == "anthropic") {
                json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    if (event_type == "message_start" && data.contains("message")) {
                        stream_id_ = data["message"].value("id", request_id_);
                        if (data["message"].contains("usage") && data["message"]["usage"].is_object()) {
                            in_tokens = data["message"]["usage"].value("input_tokens", 0);
                        }
                    } else if (event_type == "content_block_delta" && data.contains("delta")) {
                        const std::string delta_type = data["delta"].value("type", "");
                        if (delta_type == "text_delta" && data["delta"].contains("text")) {
                            text_delta = data["delta"]["text"].get<std::string>();
                        } else if (delta_type == "thinking_delta" && data["delta"].contains("thinking")) {
                            reasoning_delta += data["delta"]["thinking"].get<std::string>();
                        }
                    } else if (event_type == "message_delta" && data.contains("delta")) {
                        std::string stop = data["delta"].value("stop_reason", "stop");
                        finish_reason = (stop == "max_tokens") ? "length" : "stop";
                        if (data.contains("usage") && data["usage"].is_object()) {
                            out_tokens = data["usage"].value("output_tokens", 0);
                        }
                    } else if (event_type == "message_stop") {
                        is_done = true;
                    }
                }
            } else if (from_proto_ == "gemini") {
                json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    if (data.contains("candidates") && data["candidates"].is_array() && !data["candidates"].empty()) {
                        const auto &cand = data["candidates"][0];
                        if (cand.contains("content") && cand["content"].contains("parts")) {
                            for (const auto &p : cand["content"]["parts"]) {
                                if (!p.contains("text")) {
                                    continue;
                                }
                                if (p.value("thought", false)) {
                                    reasoning_delta += p["text"].get<std::string>();
                                } else {
                                    text_delta += p["text"].get<std::string>();
                                }
                            }
                        }
                        if (cand.contains("finishReason") && cand["finishReason"].is_string()) {
                            std::string fr = cand["finishReason"].get<std::string>();
                            finish_reason = (fr == "MAX_TOKENS") ? "length" : "stop";
                            is_done = true;
                        }
                    }
                    if (data.contains("usageMetadata") && data["usageMetadata"].is_object()) {
                        in_tokens = data["usageMetadata"].value("promptTokenCount", 0);
                        out_tokens = data["usageMetadata"].value("candidatesTokenCount", 0);
                    }
                }
            }

            if (in_tokens > 0) input_tokens_ = in_tokens;
            if (out_tokens > 0) output_tokens_ = out_tokens;
            if (!finish_reason.empty()) pending_finish_reason_ = finish_reason;
            if (!response_error_.is_null() && to_proto_ != "openai_responses") {
                const json error = {{"error", response_error_}};
                out += (to_proto_ == "anthropic" ? "event: error\ndata: " : "data: ") + dumpJson(error) + "\n\n";
                finished_ = true;
                continue;
            }
            if (to_proto_ == "openai") {
                if (!sent_role_) {
                    sent_role_ = true;
                    json chunk_obj = json::object();
                    chunk_obj["id"] = "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_);
                    chunk_obj["object"] = "chat.completion.chunk";
                    chunk_obj["created"] = static_cast<long long>(nowUnix());
                    chunk_obj["model"] = model_;
                    chunk_obj["choices"] = json::array({
                        {
                            {"index", 0},
                            {"delta", {{"role", "assistant"}, {"content", ""}}},
                            {"finish_reason", nullptr}
                        }
                    });
                    out += "data: " + dumpJson(chunk_obj) + "\n\n";
                }
                if (!reasoning_delta.empty()) {
                    json chunk_obj = json::object();
                    chunk_obj["id"] = "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_);
                    chunk_obj["object"] = "chat.completion.chunk";
                    chunk_obj["created"] = static_cast<long long>(nowUnix());
                    chunk_obj["model"] = model_;
                    chunk_obj["choices"] = json::array({
                        {
                            {"index", 0},
                            {"delta", {{"reasoning_content", reasoning_delta}}},
                            {"finish_reason", nullptr}
                        }
                    });
                    out += "data: " + dumpJson(chunk_obj) + "\n\n";
                }
                if (!text_delta.empty()) {
                    json chunk_obj = json::object();
                    chunk_obj["id"] = "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_);
                    chunk_obj["object"] = "chat.completion.chunk";
                    chunk_obj["created"] = static_cast<long long>(nowUnix());
                    chunk_obj["model"] = model_;
                    chunk_obj["choices"] = json::array({
                        {
                            {"index", 0},
                            {"delta", {{"content", text_delta}}},
                            {"finish_reason", nullptr}
                        }
                    });
                    out += "data: " + dumpJson(chunk_obj) + "\n\n";
                }
                if (!tool_calls_delta.empty()) {
                    json chunk_obj = json::object();
                    chunk_obj["id"] = "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_);
                    chunk_obj["object"] = "chat.completion.chunk";
                    chunk_obj["created"] = static_cast<long long>(nowUnix());
                    chunk_obj["model"] = model_;
                    chunk_obj["choices"] = json::array({
                        {
                            {"index", 0},
                            {"delta", {{"tool_calls", tool_calls_delta}}},
                            {"finish_reason", nullptr}
                        }
                    });
                    out += "data: " + dumpJson(chunk_obj) + "\n\n";
                }
                if (!finish_reason.empty()) {
                    json chunk_obj = json::object();
                    chunk_obj["id"] = "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_);
                    chunk_obj["object"] = "chat.completion.chunk";
                    chunk_obj["created"] = static_cast<long long>(nowUnix());
                    chunk_obj["model"] = model_;
                    chunk_obj["choices"] = json::array({
                        {
                            {"index", 0},
                            {"delta", json::object()},
                            {"finish_reason", finish_reason}
                        }
                    });
                    out += "data: " + dumpJson(chunk_obj) + "\n\n";
                }
                if (is_done) {
                    if (from_proto_ == "openai_responses" && !response_usage_.empty()) {
                        json usage = {{"id", "chatcmpl-" + (stream_id_.empty() ? request_id_ : stream_id_)},
                                      {"object", "chat.completion.chunk"}, {"created", static_cast<long long>(nowUnix())},
                                      {"model", model_}, {"choices", json::array()},
                                      {"usage", responseUsageToChat(response_usage_)}};
                        out += "data: " + dumpJson(usage) + "\n\n";
                    }
                    out += "data: [DONE]\n\n";
                    finished_ = true;
                }
            } else if (to_proto_ == "anthropic") {
                if (!sent_anthropic_start_) {
                    sent_anthropic_start_ = true;
                    json msg_start = {
                        {"type", "message_start"},
                        {"message",
                         {
                             {"id", "msg_" + (stream_id_.empty() ? request_id_ : stream_id_)},
                             {"type", "message"},
                             {"role", "assistant"},
                             {"content", json::array()},
                             {"model", model_},
                             {"stop_reason", nullptr},
                             {"stop_sequence", nullptr},
                             {"usage", {{"input_tokens", in_tokens}, {"output_tokens", 1}}},
                         }},
                    };
                    out += "event: message_start\ndata: " + dumpJson(msg_start) + "\n\n";
                }
                // Blocks are opened lazily and closed in order, because a
                // reasoning delta and a text delta can arrive in either order
                // and Anthropic wants the thinking block before the text block.
                // Each transition emits a real content_block_stop for the old
                // index and a content_block_start for the new one; a client that
                // tracks block indices sees a well-formed sequence instead of
                // deltas addressed to a block it never saw opened.
                const auto close_block = [&out, this] {
                    json block_stop = {{"type", "content_block_stop"},
                                       {"index", anthropic_index_}};
                    out += "event: content_block_stop\ndata: " + dumpJson(block_stop) + "\n\n";
                    ++anthropic_index_;
                    anthropic_block_open_ = false;
                    anthropic_block_is_thinking_ = false;
                };
                const auto open_block = [&out, this](const char *type) {
                    json block_start = json::object();
                    block_start["type"] = "content_block_start";
                    block_start["index"] = anthropic_index_;
                    if (std::string_view{type} == "thinking") {
                        block_start["content_block"] = {{"type", "thinking"}, {"thinking", ""}};
                        anthropic_block_is_thinking_ = true;
                    } else {
                        block_start["content_block"] = {{"type", "text"}, {"text", ""}};
                        anthropic_block_is_thinking_ = false;
                    }
                    out += "event: content_block_start\ndata: " + dumpJson(block_start) + "\n\n";
                    anthropic_block_open_ = true;
                    anthropic_any_block_ = true;
                };
                if (!reasoning_delta.empty()) {
                    if (anthropic_block_open_ && !anthropic_block_is_thinking_) {
                        // Reasoning that arrives after text starts still gets its
                        // own block: dropping it would silently discard content
                        // the client asked to be able to read.
                        close_block();
                    }
                    if (!anthropic_block_open_) {
                        open_block("thinking");
                    }
                    json block_delta = {
                        {"type", "content_block_delta"},
                        {"index", anthropic_index_},
                        {"delta", {{"type", "thinking_delta"}, {"thinking", reasoning_delta}}},
                    };
                    out += "event: content_block_delta\ndata: " + dumpJson(block_delta) + "\n\n";
                }
                if (!text_delta.empty()) {
                    if (anthropic_block_open_ && anthropic_block_is_thinking_) {
                        close_block();
                    }
                    if (!anthropic_block_open_) {
                        open_block("text");
                    }
                    json block_delta = {
                        {"type", "content_block_delta"},
                        {"index", anthropic_index_},
                        {"delta", {{"type", "text_delta"}, {"text", text_delta}}},
                    };
                    out += "event: content_block_delta\ndata: " + dumpJson(block_delta) + "\n\n";
                }
                if (is_done || !finish_reason.empty()) {
                    if (!anthropic_any_block_) {
                        // An answer with no content at all still owes the client
                        // the block shape it was promised when message_start
                        // went out.
                        open_block("text");
                    }
                    if (anthropic_block_open_) {
                        close_block();
                    }
                    std::string stop = (finish_reason == "length") ? "max_tokens" : "end_turn";
                    json msg_delta = {
                        {"type", "message_delta"},
                        {"delta", {{"stop_reason", stop}, {"stop_sequence", nullptr}}},
                        {"usage", {{"output_tokens", out_tokens}}},
                    };
                    out += "event: message_delta\ndata: " + dumpJson(msg_delta) + "\n\n";
                    json msg_stop = {{"type", "message_stop"}};
                    out += "event: message_stop\ndata: " + dumpJson(msg_stop) + "\n\n";
                    finished_ = true;
                }
            } else if (to_proto_ == "openai_responses") {
                startResponse(out);
                if (!reasoning_delta.empty()) {
                    const int index = ensureResponseTextItem(out, true);
                    auto &item = responses_output_[index];
                    item["summary"][0]["text"] = item["summary"][0]["text"].get<std::string>() + reasoning_delta;
                    emitResponseEvent(out, "response.reasoning_summary_text.delta",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"summary_index", 0}, {"delta", reasoning_delta}});
                }
                if (!text_delta.empty()) {
                    const int index = ensureResponseTextItem(out, false);
                    auto &item = responses_output_[index];
                    item["content"][0]["text"] = item["content"][0]["text"].get<std::string>() + text_delta;
                    emitResponseEvent(out, "response.output_text.delta",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"content_index", 0},
                                       {"delta", text_delta}, {"logprobs", json::array()}});
                }
                for (const auto &call : tool_calls_delta) appendChatCall(out, call);
                if (is_done) out += completeResponse();
            } else if (to_proto_ == "gemini") {
                if (!text_delta.empty()) {
                    json gem = {
                        {"candidates", json::array({
                            {
                                {"content", {{"parts", json::array({ {{"text", text_delta}} })}, {"role", "model"}}},
                                {"index", 0}
                            }
                        })}
                    };
                    out += "data: " + dumpJson(gem) + "\n\n";
                }
                if (is_done || !finish_reason.empty()) {
                    std::string fr = (finish_reason == "length") ? "MAX_TOKENS" : "STOP";
                    json gem = {
                        {"candidates", json::array({
                            {
                                {"content", {{"parts", json::array({ {{"text", ""}} })}, {"role", "model"}}},
                                {"finishReason", fr},
                                {"index", 0}
                            }
                        })}
                    };
                    out += "data: " + dumpJson(gem) + "\n\n";
                    finished_ = true;
                }
            }
        }
        return out;
    }

    std::string finish() {
        if (from_proto_ == to_proto_ || finished_) return {};
        if (to_proto_ == "openai_responses") return completeResponse();
        if (!finished_) {
            finished_ = true;
            if (to_proto_ == "openai") {
                return "data: [DONE]\n\n";
            } else if (to_proto_ == "anthropic") {
                // The same block bookkeeping the streaming branch uses, so a
                // stream cut short between chunks still closes the index it had
                // actually opened rather than a hard-coded zero.
                std::string out;
                if (!anthropic_any_block_) {
                    out += "event: content_block_start\ndata: {\"type\":\"content_block_start\","
                           "\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
                } else if (!anthropic_block_open_) {
                    // Everything already closed; nothing left to terminate.
                    return out;
                }
                out += std::format(
                    "event: content_block_stop\ndata: {{\"type\":\"content_block_stop\","
                    "\"index\":{}}}\n\n",
                    anthropic_index_);
                out += "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{"
                       "\"stop_reason\":\"end_turn\",\"stop_sequence\":null}}\n\n"
                       "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
                return out;
            } else if (to_proto_ == "gemini") {
                return "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\"}],\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}]}\n\n";
            }
        }
        return {};
    }

private:
    struct IncomingResponseCall {
        int index = -1;
        std::string id;
        std::string name;
        std::string arguments;
    };

    void appendResponseCall(json &deltas, int output_index, const json &item, bool snapshot) {
        auto &call = incoming_response_calls_[output_index];
        json delta = json::object();
        json fn = json::object();
        if (call.index < 0) {
            call.index = tool_index_++;
            call.id = item.value("call_id", item.value("id", "call_" + hexId(8)));
            call.name = item.value("name", "");
            delta["id"] = call.id;
            delta["type"] = "function";
            fn["name"] = call.name;
        }
        const std::string arguments = item.value("arguments", "");
        // added/done/completed carry full arguments; delta carries only the
        // next fragment. Comparing a snapshot to the accumulated prefix keeps
        // the done event from appending the whole JSON a second time.
        std::string fragment = arguments;
        if (snapshot) {
            fragment = arguments.starts_with(call.arguments) ? arguments.substr(call.arguments.size()) : std::string{};
        }
        if (!fragment.empty()) {
            call.arguments += fragment;
            fn["arguments"] = fragment;
        } else if (delta.contains("id")) {
            fn["arguments"] = "";
        }
        if (fn.empty()) return;
        delta["index"] = call.index;
        delta["function"] = std::move(fn);
        deltas.push_back(std::move(delta));
    }

    void emitResponseEvent(std::string &out, const char *type, json event) {
        event["type"] = type;
        event["sequence_number"] = response_sequence_++;
        out += std::string{"event: "} + type + "\ndata: " + dumpJson(event) + "\n\n";
    }

    json responseSnapshot(std::string_view status) const {
        json usage = response_usage_;
        if (usage.empty()) {
            usage = {{"input_tokens", input_tokens_}, {"output_tokens", output_tokens_},
                     {"total_tokens", input_tokens_ + output_tokens_}};
        }
        json response = {{"id", "resp_" + request_id_}, {"object", "response"},
                         {"created_at", response_created_}, {"status", std::string{status}}, {"model", model_},
                         {"output", responses_output_}, {"error", response_error_},
                         {"incomplete_details", nullptr}, {"usage", std::move(usage)}};
        if (status == "incomplete") {
            response["incomplete_details"] = {{"reason", pending_finish_reason_ == "content_filter" ? "content_filter" : "max_output_tokens"}};
        }
        return response;
    }

    void startResponse(std::string &out) {
        if (sent_responses_start_) return;
        sent_responses_start_ = true;
        emitResponseEvent(out, "response.created", {{"response", responseSnapshot("in_progress")}});
        emitResponseEvent(out, "response.in_progress", {{"response", responseSnapshot("in_progress")}});
    }

    int ensureResponseTextItem(std::string &out, bool reasoning) {
        int &index = reasoning ? responses_reasoning_index_ : responses_message_index_;
        if (index >= 0) return index;
        index = static_cast<int>(responses_output_.size());
        json item;
        if (reasoning) {
            item = {{"id", "rs_" + request_id_}, {"type", "reasoning"}, {"summary", json::array()}};
        } else {
            item = {{"id", "msg_" + request_id_}, {"type", "message"}, {"role", "assistant"},
                    {"status", "in_progress"}, {"content", json::array()}};
        }
        responses_output_.push_back(item);
        emitResponseEvent(out, "response.output_item.added", {{"output_index", index}, {"item", item}});
        if (reasoning) {
            json part = {{"type", "summary_text"}, {"text", ""}};
            responses_output_[index]["summary"].push_back(part);
            emitResponseEvent(out, "response.reasoning_summary_part.added",
                              {{"item_id", item["id"]}, {"output_index", index}, {"summary_index", 0}, {"part", part}});
        } else {
            json part = {{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}, {"logprobs", json::array()}};
            responses_output_[index]["content"].push_back(part);
            emitResponseEvent(out, "response.content_part.added",
                              {{"item_id", item["id"]}, {"output_index", index}, {"content_index", 0}, {"part", part}});
        }
        return index;
    }

    void appendChatCall(std::string &out, const json &call) {
        if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) return;
        const int chat_index = call.value("index", 0);
        const auto &fn = call["function"];
        auto found = responses_tool_indices_.find(chat_index);
        if (found == responses_tool_indices_.end()) {
            const int output_index = static_cast<int>(responses_output_.size());
            found = responses_tool_indices_.emplace(chat_index, output_index).first;
            json item = {{"id", "fc_" + request_id_ + "_" + std::to_string(chat_index)},
                         {"type", "function_call"}, {"status", "in_progress"},
                         {"call_id", call.contains("id") && call["id"].is_string() ? call["id"].get<std::string>() : "call_" + hexId(8)},
                         {"name", fn.contains("name") && fn["name"].is_string() ? fn["name"].get<std::string>() : std::string{}},
                         {"arguments", ""}};
            responses_output_.push_back(item);
            emitResponseEvent(out, "response.output_item.added", {{"output_index", output_index}, {"item", item}});
        } else {
            auto &item = responses_output_[found->second];
            if (call.contains("id") && call["id"].is_string()) item["call_id"] = call["id"];
            if (fn.contains("name") && fn["name"].is_string()) item["name"] = item["name"].get<std::string>() + fn["name"].get<std::string>();
        }
        auto &item = responses_output_[found->second];
        const std::string fragment = fn.contains("arguments") && fn["arguments"].is_string()
                                         ? fn["arguments"].get<std::string>() : std::string{};
        if (!fragment.empty()) {
            item["arguments"] = item["arguments"].get<std::string>() + fragment;
            emitResponseEvent(out, "response.function_call_arguments.delta",
                              {{"item_id", item["id"]}, {"output_index", found->second}, {"delta", fragment}});
        }
    }

    std::string completeResponse() {
        if (finished_) return {};
        std::string out;
        startResponse(out);
        const bool incomplete = pending_finish_reason_ == "length" || pending_finish_reason_ == "content_filter";
        const std::string status = !response_error_.is_null() ? "failed" : incomplete ? "incomplete" : "completed";
        for (std::size_t index = 0; index < responses_output_.size(); ++index) {
            auto &item = responses_output_[index];
            const std::string type = item.value("type", "");
            if (type == "message") {
                for (std::size_t content_index = 0; content_index < item["content"].size(); ++content_index) {
                    const auto &part = item["content"][content_index];
                    emitResponseEvent(out, "response.output_text.done",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"content_index", content_index},
                                       {"text", part.value("text", "")}, {"logprobs", json::array()}});
                    emitResponseEvent(out, "response.content_part.done",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"content_index", content_index}, {"part", part}});
                }
                item["status"] = incomplete || status == "failed" ? "incomplete" : "completed";
            } else if (type == "reasoning") {
                for (std::size_t summary_index = 0; summary_index < item["summary"].size(); ++summary_index) {
                    const auto &part = item["summary"][summary_index];
                    emitResponseEvent(out, "response.reasoning_summary_text.done",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"summary_index", summary_index},
                                       {"text", part.value("text", "")}});
                    emitResponseEvent(out, "response.reasoning_summary_part.done",
                                      {{"item_id", item["id"]}, {"output_index", index}, {"summary_index", summary_index}, {"part", part}});
                }
            } else if (type == "function_call") {
                emitResponseEvent(out, "response.function_call_arguments.done",
                                  {{"item_id", item["id"]}, {"output_index", index}, {"name", item["name"]}, {"arguments", item["arguments"]}});
                item["status"] = incomplete || status == "failed" ? "incomplete" : "completed";
            }
            emitResponseEvent(out, "response.output_item.done", {{"output_index", index}, {"item", item}});
        }
        const char *event = status == "failed" ? "response.failed" : incomplete ? "response.incomplete" : "response.completed";
        emitResponseEvent(out, event, {{"response", responseSnapshot(status)}});
        finished_ = true;
        return out;
    }

    // ── AWS event stream ─────────────────────────────────────────────────────
    //
    // Bedrock's streaming API is not SSE. Each message is
    //   [total_len:4][headers_len:4][prelude_crc:4][headers][payload][crc:4]
    // big-endian, with a CRC32 over the prelude and another over everything
    // before the trailing CRC. A frame whose checksums do not match is a
    // damaged stream, and guessing at its boundaries would turn framing
    // damage into plausible-looking model output.
    static std::uint32_t crc32Of(const char *data, std::size_t length) {
        static const std::array<std::uint32_t, 256> table = [] {
            std::array<std::uint32_t, 256> out{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t value = i;
                for (int bit = 0; bit < 8; ++bit) {
                    value = (value & 1) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
                }
                out[i] = value;
            }
            return out;
        }();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < length; ++i) {
            crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFFu] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    std::uint32_t readU32(std::size_t at) const {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[at])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[at + 1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[at + 2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[at + 3]));
    }

    // Consumes one frame. Returns false when the buffer does not yet hold a whole
    // one (the caller waits for more bytes) — or when the frame is corrupt, in
    // which case `bedrock_broken_` stops the stream instead of resynchronising
    // on whatever looks like a length.
    bool nextBedrockEvent(std::string &data_out, std::string &event_type_out) {
        if (buffer_.size() < 16) return false;
        const std::uint32_t total = readU32(0);
        const std::uint32_t header_length = readU32(4);
        if (total < 16 || header_length > total - 16) {
            bedrock_broken_ = true;
            return false;
        }
        if (buffer_.size() < total) return false;
        if (readU32(8) != crc32Of(buffer_.data(), 8) ||
            readU32(total - 4) != crc32Of(buffer_.data(), total - 4)) {
            bedrock_broken_ = true;
            return false;
        }

        const std::size_t headers_end = 12 + header_length;
        std::size_t at = 12;
        std::string message_type = "event";
        std::string event_type;
        while (at < headers_end) {
            const std::size_t name_length = static_cast<unsigned char>(buffer_[at++]);
            if (at + name_length + 1 > headers_end) break;
            const std::string name = buffer_.substr(at, name_length);
            at += name_length;
            const unsigned char type = static_cast<unsigned char>(buffer_[at++]);
            const auto read_string = [&]() -> std::string {
                if (at + 2 > headers_end) return {};
                const std::size_t length =
                    (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[at])) << 8) |
                    static_cast<unsigned char>(buffer_[at + 1]);
                at += 2;
                if (at + length > headers_end) return {};
                std::string value = buffer_.substr(at, length);
                at += length;
                return value;
            };
            std::string value;
            switch (type) {
            case 0: value = "true"; break;
            case 1: value = "false"; break;
            case 2: at += 1; break;
            case 3: at += 2; break;
            case 4: at += 4; break;
            case 5: at += 8; break;
            case 6:
                if (at + 2 <= headers_end) {
                    const std::size_t length =
                        (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[at])) << 8) |
                        static_cast<unsigned char>(buffer_[at + 1]);
                    at += 2 + length;
                }
                break;
            case 7: value = read_string(); break;
            case 8: at += 8; break;
            case 9: at += 16; break;
            default: at = headers_end; break;
            }
            if (name == ":message-type") message_type = value;
            else if (name == ":event-type") event_type = value;
            else if (name == ":exception-type") event_type = value;
        }

        const std::size_t payload_length = total - 4 - headers_end;
        data_out = buffer_.substr(headers_end, payload_length);
        event_type_out = message_type == "event" ? event_type : message_type + ":" + event_type;
        buffer_.erase(0, total);
        return true;
    }

    std::string from_proto_;
    std::string to_proto_;
    std::string model_;
    std::string request_id_;
    std::string stream_id_;
    std::string buffer_;
    bool finished_ = false;
    bool sent_role_ = false;
    bool sent_anthropic_start_ = false;
    bool sent_responses_start_ = false;
    const long long response_created_ = static_cast<long long>(nowUnix());
    int response_sequence_ = 0;
    int responses_message_index_ = -1;
    int responses_reasoning_index_ = -1;
    int input_tokens_ = 0;
    int output_tokens_ = 0;
    std::string pending_finish_reason_;
    json responses_output_ = json::array();
    json response_usage_ = json::object();
    json response_error_ = nullptr;
    std::map<int, int> responses_tool_indices_;
    std::map<int, IncomingResponseCall> incoming_response_calls_;
    // Anthropic content-block bookkeeping: which index is open, whether it is
    // the thinking block, and whether any block was opened at all (an answer
    // with no content still has to be closed properly).
    int anthropic_index_ = 0;
    bool anthropic_block_open_ = false;
    bool anthropic_block_is_thinking_ = false;
    bool anthropic_any_block_ = false;
    // Bedrock/OpenAI tool-call bookkeeping. The index is what OpenAI clients use
    // to stitch streamed argument fragments onto the right call.
    int tool_index_ = 0;
    std::string tool_id_;
    std::string tool_name_;
    bool bedrock_broken_ = false;
};

StreamProtocolAdapter::StreamProtocolAdapter(const ProviderConfig &provider,
                                             std::string model,
                                             std::string request_id)
    : impl_(std::make_unique<Impl>(provider.protocol, "openai", std::move(model), std::move(request_id))) {}

StreamProtocolAdapter::StreamProtocolAdapter(std::string from_protocol,
                                             std::string to_protocol,
                                             std::string model,
                                             std::string request_id)
    : impl_(std::make_unique<Impl>(std::move(from_protocol), std::move(to_protocol), std::move(model), std::move(request_id))) {}

StreamProtocolAdapter::~StreamProtocolAdapter() = default;

StreamProtocolAdapter::StreamProtocolAdapter(StreamProtocolAdapter &&) noexcept = default;
StreamProtocolAdapter &StreamProtocolAdapter::operator=(StreamProtocolAdapter &&) noexcept = default;

std::string StreamProtocolAdapter::feed(std::string_view chunk) {
    return impl_->feed(chunk);
}

std::string StreamProtocolAdapter::finish() {
    return impl_->finish();
}

// ── stream usage ────────────────────────────────────────────────────────────

namespace {

// Far more than one event of any of the four protocols needs, and the bound on
// what an unterminated one can cost.
constexpr std::size_t kUsageCarryLimit = 8192;

// The two numbers under every spelling these protocols use for them. A relay
// advertising "OpenAI-compatible" may still answer with the newer
// input/output naming, so both are read wherever a usage object turns up.
void pullUsage(const json &node, std::uint64_t &prompt, std::uint64_t &completion, bool &reported) {
    if (!node.is_object()) {
        return;
    }
    const auto count = [&node, &reported](const char *key) -> std::uint64_t {
        const auto it = node.find(key);
        if (it == node.end()) {
            return 0;
        }
        if (it->is_number_unsigned()) {
            reported = true;
            return it->get<std::uint64_t>();
        }
        if (it->is_number_integer() && it->get<long long>() >= 0) {
            reported = true;
            return static_cast<std::uint64_t>(it->get<long long>());
        }
        if (it->is_number_float()) {
            const double value = it->get<double>();
            if (value >= 0 && value < 18446744073709551616.0 && std::floor(value) == value) {
                reported = true;
                return static_cast<std::uint64_t>(value);
            }
        }
        // A relay that sends the count as a string is not worth guessing at.
        return 0;
    };
    prompt = std::max(prompt, std::max(count("prompt_tokens"), count("input_tokens")));
    completion =
        std::max(completion, std::max(count("completion_tokens"), count("output_tokens")));
}

// Anthropic and the Responses API both nest the usage object one level down,
// under an envelope that carries the event's own name.
void pullNested(const json &root, const char *key, std::uint64_t &prompt,
                std::uint64_t &completion, bool &reported) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        return;
    }
    pullUsage(*it, prompt, completion, reported);
    if (const auto inner = it->find("usage"); inner != it->end()) {
        pullUsage(*inner, prompt, completion, reported);
    }
}

void absorbDocument(const json &root, std::uint64_t &prompt, std::uint64_t &completion, bool &reported) {
    if (root.is_array()) {
        // Gemini without ?alt=sse answers with an array of the same objects,
        // and the proxy cannot assume the query string survived the relay.
        for (const auto &item : root) {
            absorbDocument(item, prompt, completion, reported);
        }
        return;
    }
    if (!root.is_object()) {
        return;
    }
    pullUsage(root, prompt, completion, reported);
    if (const auto it = root.find("usage"); it != root.end()) {
        pullUsage(*it, prompt, completion, reported);
    }
    pullNested(root, "message", prompt, completion, reported);
    pullNested(root, "response", prompt, completion, reported);
    if (const auto it = root.find("usageMetadata"); it != root.end() && it->is_object()) {
        // Gemini spells them its own way, and reports them cumulatively on
        // every chunk — which is why the merge is a max everywhere.
        const auto count = [&it, &reported](const char *key) -> std::uint64_t {
            const auto field = it->find(key);
            if (field != it->end() && field->is_number_unsigned()) {
                reported = true;
                return field->get<std::uint64_t>();
            }
            return 0;
        };
        prompt = std::max(prompt, count("promptTokenCount"));
        completion = std::max(completion, count("candidatesTokenCount"));
    }
}

} // namespace

void StreamUsageObserver::absorbLine(std::string_view line) {
    // The cheap test that decides whether any of the work below runs at all.
    if (line.find("usage") == std::string_view::npos) {
        return;
    }
    if (startsWith(line, "event:") || startsWith(line, ":")) {
        return;
    }
    if (startsWith(line, "data:")) {
        line.remove_prefix(5);
    }
    const std::string body{trim(line)};
    if (body.empty() || body == "[DONE]") {
        return;
    }
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) {
        return;
    }
    absorbDocument(root, prompt_, completion_, reported_);
}

void StreamUsageObserver::feed(std::string_view chunk) {
    if (chunk.empty()) {
        return;
    }
    // The fast path, and the reason this class is cheap enough to sit on the hot
    // path of every streamed answer: a chunk that ends on an event boundary and
    // mentions no usage leaves nothing to carry, so nothing is copied and no line
    // is looked at. A chunk that does NOT end on one — an event split across two
    // reads, or the word "usage" itself split — takes the slow path below, where
    // the tail is kept until the event completes.
    if (carry_.empty() && chunk.back() == '\n' && chunk.find("usage") == std::string_view::npos) {
        return;
    }
    carry_.append(chunk);
    // Whole lines only: an event split across two chunks is completed by the
    // next feed, which is the whole reason the tail is kept.
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = carry_.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }
        absorbLine(std::string_view{carry_}.substr(start, newline - start));
        start = newline + 1;
    }
    carry_.erase(0, start);
    if (carry_.size() > kUsageCarryLimit) {
        carry_.erase(0, carry_.size() - kUsageCarryLimit);
    }
}

TokenUsage StreamUsageObserver::usage() const {
    return TokenUsage{.prompt = prompt_, .completion = completion_, .reported = reported_};
}

bool tokenUsageReported(std::string_view body) {
    const auto root = json::parse(body, nullptr, false);
    // Buffered accounting only reads these top-level envelopes. Stream events
    // have additional nesting; recognizing it here would refund an uncounted
    // response as though it explicitly reported zero tokens.
    const auto reported = [](const json &node) {
        if (!node.is_object()) return false;
        if (const auto usage = node.find("usage"); usage != node.end() && usage->is_object()) {
            for (const auto *key : {"prompt_tokens", "input_tokens", "completion_tokens", "output_tokens"}) {
                const auto value = usage->find(key);
                if (value == usage->end()) continue;
                if (value->is_number_unsigned()) return true;
                if (value->is_number_float()) {
                    const double n = value->get<double>();
                    if (n >= 0 && n < 18446744073709551616.0 && std::floor(n) == n) return true;
                }
            }
        }
        if (const auto usage = node.find("usageMetadata"); usage != node.end() && usage->is_object()) {
            for (const auto *key : {"promptTokenCount", "candidatesTokenCount"}) {
                const auto value = usage->find(key);
                if (value != usage->end() && value->is_number_unsigned()) return true;
            }
        }
        return false;
    };
    if (root.is_object()) return reported(root);
    if (root.is_array()) {
        for (const auto &item : root) {
            if (reported(item)) return true;
        }
    }
    return false;
}

} // namespace literouter
