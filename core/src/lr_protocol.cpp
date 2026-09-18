// Protocol adapters: bidirectional translation between OpenAI format and
// upstream provider protocols (Anthropic Claude Messages API, Google Gemini,
// and OpenAI Responses API).

module;

#include <httplib.h>

module literouter.core;

import std;
import nlohmann.json;

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

} // namespace

std::string resolveChatPath(const ProviderConfig &provider,
                            std::string_view upstream_model,
                            bool stream) {
    if (isGemini(provider)) {
        if (!provider.chat_path.empty() && provider.chat_path != "/chat/completions") {
            return provider.chat_path;
        }
        if (stream) {
            return std::format("/v1beta/models/{}:streamGenerateContent?alt=sse", upstream_model);
        }
        return std::format("/v1beta/models/{}:generateContent", upstream_model);
    }
    if (isAnthropic(provider)) {
        if (provider.chat_path == "/chat/completions") {
            return "/v1/messages";
        }
        return provider.chat_path;
    }
    if (isOpenAiResponses(provider)) {
        if (provider.chat_path == "/chat/completions") {
            return "/v1/responses";
        }
        return provider.chat_path;
    }
    return provider.chat_path;
}

std::string resolveModelsPath(const ProviderConfig &provider) {
    if (isGemini(provider)) {
        return "/v1beta/models";
    }
    if (isAnthropic(provider)) {
        return "/v1/models";
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

    if (isAnthropic(provider)) {
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
                        tool_res["content"] = m.contains("content")
                            ? (m["content"].is_string() ? m["content"].get<std::string>() : m["content"].dump())
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
        return anthropic.dump();
    }

    if (isGemini(provider)) {
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
        return gemini.dump();
    }

    if (isOpenAiResponses(provider)) {
        json resp = json::object();
        resp["model"] = std::string{upstream_model};
        resp["stream"] = stream;
        if (req.contains("messages")) {
            resp["input"] = req["messages"];
        }
        if (req.contains("temperature")) resp["temperature"] = req["temperature"];
        if (req.contains("max_tokens")) resp["max_output_tokens"] = req["max_tokens"];
        return resp.dump();
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
    return out.dump();
}

std::string adaptChatResponse(const ProviderConfig &provider,
                              std::string_view upstream_response,
                              std::string_view requested_model) {
    if (!isAnthropic(provider) && !isGemini(provider) && !isOpenAiResponses(provider)) {
        return std::string{upstream_response};
    }

    const json root = json::parse(upstream_response, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.contains("error")) {
        return std::string{upstream_response};
    }

    if (isAnthropic(provider)) {
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
                    fn["arguments"] = block.contains("input") ? block["input"].dump() : "{}";
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
        return out.dump();
    }

    if (isGemini(provider)) {
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
        return out.dump();
    }

    if (isOpenAiResponses(provider)) {
        json choice = json::object();
        choice["index"] = 0;
        json message = json::object();
        message["role"] = "assistant";
        std::string text_content;
        std::string reasoning_content;

        if (root.contains("output") && root["output"].is_array()) {
            for (const auto &item : root["output"]) {
                if (item.value("type", "") == "reasoning") {
                    // A reasoning item carries its readable text as a summary;
                    // `content` is where an unencrypted relay puts it instead.
                    // Both are read because both shapes are in the wild, and an
                    // encrypted one simply contributes nothing.
                    if (const auto summary = item.find("summary");
                        summary != item.end() && summary->is_array()) {
                        for (const auto &s : *summary) {
                            reasoning_content += s.value("text", std::string{});
                        }
                    }
                    if (const auto content = item.find("content");
                        content != item.end() && content->is_array()) {
                        for (const auto &c : *content) {
                            reasoning_content += c.value("text", std::string{});
                        }
                    }
                    continue;
                }
                if (item.value("type", "") == "message" && item.contains("content") && item["content"].is_array()) {
                    for (const auto &c : item["content"]) {
                        if (c.value("type", "") == "text" && c.contains("text")) {
                            text_content += c["text"].get<std::string>();
                        }
                    }
                }
            }
        }
        message["content"] = text_content;
        if (!reasoning_content.empty()) {
            message["reasoning_content"] = reasoning_content;
        }
        choice["message"] = message;
        choice["finish_reason"] = "stop";

        json out = json::object();
        out["id"] = "chatcmpl-" + root.value("id", hexId(12));
        out["object"] = "chat.completion";
        out["created"] = root.value("created_at", static_cast<long long>(nowUnix()));
        out["model"] = std::string{requested_model};
        out["choices"] = json::array({choice});
        if (root.contains("usage")) {
            out["usage"] = root["usage"];
        }
        return out.dump();
    }

    return std::string{upstream_response};
}

std::string adaptResponsesToChat(std::string_view responses_request_json) {
    const json req = json::parse(responses_request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return std::string{responses_request_json};
    }

    json chat = json::object();
    chat["model"] = req.value("model", "");
    chat["stream"] = req.value("stream", false);

    json msgs = json::array();
    if (req.contains("input")) {
        if (req["input"].is_string()) {
            msgs.push_back({{"role", "user"}, {"content", req["input"].get<std::string>()}});
        } else if (req["input"].is_array()) {
            msgs = req["input"];
        }
    }
    chat["messages"] = msgs;
    if (req.contains("temperature")) chat["temperature"] = req["temperature"];
    if (req.contains("max_output_tokens")) chat["max_tokens"] = req["max_output_tokens"];
    return chat.dump();
}

std::string adaptChatToResponses(std::string_view chat_completion_response_json,
                                 std::string_view requested_model) {
    const json root = json::parse(chat_completion_response_json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.contains("error")) {
        return std::string{chat_completion_response_json};
    }

    std::string text;
    if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
        const auto &choice = root["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
            if (choice["message"]["content"].is_string()) {
                text = choice["message"]["content"].get<std::string>();
            }
        }
    }

    json resp = json::object();
    resp["id"] = "resp_" + root.value("id", hexId(12));
    resp["object"] = "response";
    resp["created_at"] = root.value("created", static_cast<long long>(nowUnix()));
    resp["status"] = "completed";
    resp["model"] = std::string{requested_model};
    // Reasoning first: the Responses API orders a reasoning item before the
    // message it produced, and that is also the order a client renders.
    json output = json::array();
    std::string reasoning;
    if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
        const auto &message = root["choices"][0]["message"];
        reasoning = message.value("reasoning_content", "");
        if (reasoning.empty()) {
            reasoning = message.value("reasoning", "");
        }
    }
    if (!reasoning.empty()) {
        output.push_back({
            {"id", "rs_" + hexId(12)},
            {"type", "reasoning"},
            {"summary", json::array({ {{"type", "summary_text"}, {"text", reasoning}} })}
        });
    }
    output.push_back({
        {"id", "msg_" + hexId(12)},
        {"type", "message"},
        {"role", "assistant"},
        {"content", json::array({ {{"type", "text"}, {"text", text}} })}
    });
    resp["output"] = std::move(output);
    if (root.contains("usage")) {
        resp["usage"] = root["usage"];
    }
    return resp.dump();
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
                                fn["arguments"] = block.contains("input")
                                    ? (block["input"].is_string() ? block["input"].get<std::string>() : block["input"].dump())
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
                                        : block["content"].dump();
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

    return chat.dump();
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
                        fn["arguments"] = fc.contains("args") ? fc["args"].dump() : "{}";
                        tc["function"] = fn;
                        tool_calls.push_back(tc);
                    } else if (p.contains("functionResponse")) {
                        const auto &fr = p["functionResponse"];
                        json tool_msg = json::object();
                        tool_msg["role"] = "tool";
                        tool_msg["tool_call_id"] = "call_0";
                        tool_msg["name"] = fr.value("name", "");
                        tool_msg["content"] = fr.contains("response") ? fr["response"].dump() : "{}";
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

    return chat.dump();
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

    return anthropic.dump();
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

    return gemini.dump();
}

// ── StreamProtocolAdapter ───────────────────────────────────────────────────

class StreamProtocolAdapter::Impl {
public:
    Impl(std::string from_proto, std::string to_proto, std::string model, std::string request_id)
        : from_proto_(toLower(from_proto)), to_proto_(toLower(to_proto)),
          model_(std::move(model)), request_id_(std::move(request_id)) {
        if (from_proto_.empty()) from_proto_ = "openai";
        if (to_proto_.empty()) to_proto_ = "openai";
    }

    std::string feed(std::string_view chunk) {
        if (from_proto_ == to_proto_) {
            return std::string{chunk};
        }

        buffer_.append(chunk);
        std::string out;

        for (;;) {
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

            std::string event_type;
            std::string data_str;
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

            if (data_str.empty()) continue;

            std::string text_delta;
            std::string reasoning_delta;
            std::string finish_reason;
            int out_tokens = 0;
            int in_tokens = 0;
            bool is_done = false;

            if (from_proto_ == "openai_responses") {
                // The Responses API streams named events rather than chat deltas:
                // the text arrives as `response.output_text.delta`, the reasoning
                // as `response.reasoning_summary_text.delta`, and the usage with
                // the final `response.completed`.
                const json data = json::parse(data_str, nullptr, false);
                if (!data.is_discarded() && data.is_object()) {
                    const std::string type = data.value("type", std::string{});
                    if (type == "response.output_text.delta") {
                        text_delta = data.value("delta", std::string{});
                    } else if (type == "response.reasoning_summary_text.delta" ||
                               type == "response.reasoning_text.delta") {
                        reasoning_delta = data.value("delta", std::string{});
                    } else if (type == "response.completed" || type == "response.done" ||
                               type == "response.incomplete") {
                        is_done = true;
                        if (const auto response = data.find("response");
                            response != data.end() && response->is_object()) {
                            if (const auto usage = response->find("usage");
                                usage != response->end() && usage->is_object()) {
                                in_tokens = usage->value("input_tokens", in_tokens);
                                out_tokens = usage->value("output_tokens", out_tokens);
                            }
                        }
                    } else if (type == "response.failed" || type == "error") {
                        is_done = true;
                    }
                }
            } else if (from_proto_ == "openai") {
                if (trim(data_str) == "[DONE]") {
                    is_done = true;
                } else {
                    json data = json::parse(data_str, nullptr, false);
                    if (!data.is_discarded() && data.is_object()) {
                        if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
                            const auto &choice = data["choices"][0];
                            if (choice.contains("delta") && choice["delta"].is_object()) {
                                if (choice["delta"].contains("content") && choice["delta"]["content"].is_string()) {
                                    text_delta = choice["delta"]["content"].get<std::string>();
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
                                is_done = true;
                            }
                        }
                        if (data.contains("usage") && data["usage"].is_object()) {
                            in_tokens = data["usage"].value("prompt_tokens", 0);
                            out_tokens = data["usage"].value("completion_tokens", 0);
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
                    out += "data: " + chunk_obj.dump() + "\n\n";
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
                    out += "data: " + chunk_obj.dump() + "\n\n";
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
                    out += "data: " + chunk_obj.dump() + "\n\n";
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
                    out += "data: " + chunk_obj.dump() + "\n\n";
                }
                if (is_done) {
                    out += "data: [DONE]\n\n";
                    finished_ = true;
                }
            } else if (to_proto_ == "anthropic") {
                if (!sent_anthropic_start_) {
                    sent_anthropic_start_ = true;
                    json msg_start = {
                        {"type", "message_start"},
                        {"message", {
                            {"id", "msg_" + (stream_id_.empty() ? request_id_ : stream_id_)},
                            {"type", "message"},
                            {"role", "assistant"},
                            {"content", json::array()},
                            {"model", model_},
                            {"stop_reason", nullptr},
                            {"stop_sequence", nullptr},
                            {"usage", {{"input_tokens", in_tokens}, {"output_tokens", 1}}}
                        }}
                    };
                    out += "event: message_start\ndata: " + msg_start.dump() + "\n\n";
                    json block_start = {
                        {"type", "content_block_start"},
                        {"index", 0},
                        {"content_block", {{"type", "text"}, {"text", ""}}}
                    };
                    out += "event: content_block_start\ndata: " + block_start.dump() + "\n\n";
                }
                if (!text_delta.empty()) {
                    json block_delta = {
                        {"type", "content_block_delta"},
                        {"index", 0},
                        {"delta", {{"type", "text_delta"}, {"text", text_delta}}}
                    };
                    out += "event: content_block_delta\ndata: " + block_delta.dump() + "\n\n";
                }
                if (is_done || !finish_reason.empty()) {
                    std::string stop = (finish_reason == "length") ? "max_tokens" : "end_turn";
                    json block_stop = {{"type", "content_block_stop"}, {"index", 0}};
                    out += "event: content_block_stop\ndata: " + block_stop.dump() + "\n\n";
                    json msg_delta = {
                        {"type", "message_delta"},
                        {"delta", {{"stop_reason", stop}, {"stop_sequence", nullptr}}},
                        {"usage", {{"output_tokens", out_tokens}}}
                    };
                    out += "event: message_delta\ndata: " + msg_delta.dump() + "\n\n";
                    json msg_stop = {{"type", "message_stop"}};
                    out += "event: message_stop\ndata: " + msg_stop.dump() + "\n\n";
                    finished_ = true;
                }
            } else if (to_proto_ == "openai_responses") {
                // The same three moments a Responses client expects: the response
                // begins, the deltas arrive (reasoning and text as their own
                // events), and the response completes. Without this branch the
                // stream was assembled and then thrown away, and a client asking
                // for `stream: true` on /v1/responses got an empty body.
                if (!sent_responses_start_) {
                    sent_responses_start_ = true;
                    json created = {
                        {"type", "response.created"},
                        {"response", {
                            {"id", "resp_" + (stream_id_.empty() ? request_id_ : stream_id_)},
                            {"object", "response"},
                            {"status", "in_progress"},
                            {"model", model_},
                            {"output", json::array()}
                        }}
                    };
                    out += "event: response.created\ndata: " + created.dump() + "\n\n";
                }
                if (!reasoning_delta.empty()) {
                    json event = {
                        {"type", "response.reasoning_summary_text.delta"},
                        {"delta", reasoning_delta}
                    };
                    out += "event: response.reasoning_summary_text.delta\ndata: " + event.dump() +
                           "\n\n";
                }
                if (!text_delta.empty()) {
                    json event = {
                        {"type", "response.output_text.delta"},
                        {"delta", text_delta}
                    };
                    out += "event: response.output_text.delta\ndata: " + event.dump() + "\n\n";
                }
                if (is_done || !finish_reason.empty()) {
                    json completed = {
                        {"type", "response.completed"},
                        {"response", {
                            {"id", "resp_" + (stream_id_.empty() ? request_id_ : stream_id_)},
                            {"object", "response"},
                            {"status", "completed"},
                            {"model", model_},
                            {"usage", {{"input_tokens", in_tokens}, {"output_tokens", out_tokens}}}
                        }}
                    };
                    out += "event: response.completed\ndata: " + completed.dump() + "\n\n";
                    finished_ = true;
                }
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
                    out += "data: " + gem.dump() + "\n\n";
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
                    out += "data: " + gem.dump() + "\n\n";
                    finished_ = true;
                }
            }
        }
        return out;
    }

    std::string finish() {
        if (to_proto_ == "openai_responses" && !finished_) {
            // The client is waiting for the end of a response it was promised:
            // a stream that never completes is worse than one that says it
            // stopped.
            json completed = {
                {"type", "response.completed"},
                {"response", {
                    {"id", "resp_" + (stream_id_.empty() ? request_id_ : stream_id_)},
                    {"object", "response"},
                    {"status", "completed"},
                    {"model", model_},
                    {"output", json::array()}
                }}
            };
            finished_ = true;
            return "event: response.completed\ndata: " + completed.dump() + "\n\n";
        }
        if (from_proto_ == to_proto_) {
            return {};
        }
        if (!finished_) {
            finished_ = true;
            if (to_proto_ == "openai") {
                return "data: [DONE]\n\n";
            } else if (to_proto_ == "anthropic") {
                return "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\nevent: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null}}\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
            } else if (to_proto_ == "gemini") {
                return "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\"}],\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}]}\n\n";
            }
        }
        return {};
    }

private:
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
void pullUsage(const json &node, std::uint64_t &prompt, std::uint64_t &completion) {
    if (!node.is_object()) {
        return;
    }
    const auto count = [&node](const char *key) -> std::uint64_t {
        const auto it = node.find(key);
        if (it == node.end()) {
            return 0;
        }
        if (it->is_number_unsigned()) {
            return it->get<std::uint64_t>();
        }
        if (it->is_number_integer()) {
            return static_cast<std::uint64_t>(std::max<long long>(0, it->get<long long>()));
        }
        if (it->is_number_float()) {
            const double value = it->get<double>();
            return value > 0.0 ? static_cast<std::uint64_t>(value) : 0;
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
                std::uint64_t &completion) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        return;
    }
    pullUsage(*it, prompt, completion);
    if (const auto inner = it->find("usage"); inner != it->end()) {
        pullUsage(*inner, prompt, completion);
    }
}

void absorbDocument(const json &root, std::uint64_t &prompt, std::uint64_t &completion) {
    if (root.is_array()) {
        // Gemini without ?alt=sse answers with an array of the same objects,
        // and the proxy cannot assume the query string survived the relay.
        for (const auto &item : root) {
            absorbDocument(item, prompt, completion);
        }
        return;
    }
    if (!root.is_object()) {
        return;
    }
    pullUsage(root, prompt, completion);
    if (const auto it = root.find("usage"); it != root.end()) {
        pullUsage(*it, prompt, completion);
    }
    pullNested(root, "message", prompt, completion);
    pullNested(root, "response", prompt, completion);
    if (const auto it = root.find("usageMetadata"); it != root.end() && it->is_object()) {
        // Gemini spells them its own way, and reports them cumulatively on
        // every chunk — which is why the merge is a max everywhere.
        const auto count = [&it](const char *key) -> std::uint64_t {
            const auto field = it->find(key);
            return field != it->end() && field->is_number_unsigned()
                       ? field->get<std::uint64_t>()
                       : 0;
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
    absorbDocument(root, prompt_, completion_);
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
    return TokenUsage{.prompt = prompt_, .completion = completion_};
}

} // namespace literouter
