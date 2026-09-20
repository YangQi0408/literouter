// Unit tests for upstream protocol adapters (Anthropic Claude, Google Gemini,
// OpenAI Responses, and standard OpenAI compatible).
#include "lr_test_check.h"

import nlohmann.json;
import literouter.core;

namespace {

using json = nlohmann::json;

using literouter::ProviderConfig;
using literouter::resolveChatPath;
using literouter::resolveModelsPath;
using literouter::adaptChatRequest;
using literouter::adaptChatResponse;
using literouter::adaptResponsesToChat;
using literouter::adaptChatToResponses;
using literouter::adaptAnthropicToChat;
using literouter::adaptGeminiToChat;
using literouter::adaptChatToAnthropic;
using literouter::adaptChatToGemini;
using literouter::StreamProtocolAdapter;

void testResolvePaths() {
    LR_GROUP("resolveChatPath and resolveModelsPath");

    ProviderConfig openai;
    openai.protocol = "openai";
    openai.chat_path = "/chat/completions";
    LR_CHECK_EQ(resolveChatPath(openai, "gpt-4o", false), "/chat/completions");
    LR_CHECK_EQ(resolveChatPath(openai, "gpt-4o", true), "/chat/completions");
    LR_CHECK_EQ(resolveModelsPath(openai), "/models");

    ProviderConfig anthropic;
    anthropic.protocol = "anthropic";
    anthropic.chat_path = "/chat/completions";
    LR_CHECK_EQ(resolveChatPath(anthropic, "claude-3-5-sonnet", false), "/v1/messages");
    LR_CHECK_EQ(resolveChatPath(anthropic, "claude-3-5-sonnet", true), "/v1/messages");
    LR_CHECK_EQ(resolveModelsPath(anthropic), "/v1/models");

    // Anthropic with custom path overridden
    anthropic.chat_path = "/custom/messages";
    LR_CHECK_EQ(resolveChatPath(anthropic, "claude-3-5-sonnet", false), "/custom/messages");

    ProviderConfig gemini;
    gemini.protocol = "gemini";
    LR_CHECK_EQ(resolveChatPath(gemini, "gemini-1.5-pro", false),
                "/v1beta/models/gemini-1.5-pro:generateContent");
    LR_CHECK_EQ(resolveChatPath(gemini, "gemini-1.5-pro", true),
                "/v1beta/models/gemini-1.5-pro:streamGenerateContent?alt=sse");
    LR_CHECK_EQ(resolveModelsPath(gemini), "/v1beta/models");

    ProviderConfig responses;
    responses.protocol = "openai_responses";
    responses.chat_path = "/chat/completions";
    LR_CHECK_EQ(resolveChatPath(responses, "gpt-4o", false), "/v1/responses");
}

void testAnthropicRequestAdaptation() {
    LR_GROUP("Anthropic request adaptation");

    ProviderConfig provider;
    provider.protocol = "anthropic";

    // Test system extraction and message concatenation
    std::string req = R"({
        "model": "claude-3-5-sonnet-20241022",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Hello"},
            {"role": "user", "content": "Another greeting"}
        ],
        "temperature": 0.7
    })";

    std::string adapted = adaptChatRequest(provider, "claude-3-5-sonnet-20241022", req, false);
    LR_CHECK(adapted.find(R"("system":"You are a helpful assistant.")") != std::string::npos);
    // Consecutive user messages should be merged
    LR_CHECK(adapted.find("Hello\\n\\nAnother greeting") != std::string::npos);
    // Default max_tokens should be injected
    LR_CHECK(adapted.find(R"("max_tokens":4096)") != std::string::npos);
    LR_CHECK(adapted.find(R"("model":"claude-3-5-sonnet-20241022")") != std::string::npos);

    // Test assistant leading message handling
    std::string assistant_first = R"({
        "model": "claude-3-5-sonnet",
        "messages": [
            {"role": "assistant", "content": "I am already here."}
        ]
    })";
    std::string adapted_asst = adaptChatRequest(provider, "claude-3-5-sonnet", assistant_first, false);
    // Anthropic requires user message first; our adapter inserts a dummy user message
    LR_CHECK(adapted_asst.find(R"("role":"user")") != std::string::npos);

    // Test tool conversion
    std::string tools_req = R"({
        "model": "claude-3-5-sonnet",
        "messages": [{"role": "user", "content": "Calculate 2+2"}],
        "tools": [
            {
                "type": "function",
                "function": {
                    "name": "calc",
                    "description": "Calculator",
                    "parameters": {"type": "object"}
                }
            }
        ]
    })";
    std::string adapted_tools = adaptChatRequest(provider, "claude-3-5-sonnet", tools_req, false);
    LR_CHECK(adapted_tools.find(R"("tools":)") != std::string::npos);
    LR_CHECK(adapted_tools.find(R"("input_schema":)") != std::string::npos);
    LR_CHECK(adapted_tools.find(R"("name":"calc")") != std::string::npos);
}

void testAnthropicResponseAdaptation() {
    LR_GROUP("Anthropic response adaptation");

    ProviderConfig provider;
    provider.protocol = "anthropic";

    std::string anthropic_resp = R"({
        "id": "msg_01XFDUDYJgAACzvnptvVoYEL",
        "type": "message",
        "role": "assistant",
        "content": [
            {"type": "text", "text": "Hello! How can I assist you today?"}
        ],
        "stop_reason": "end_turn",
        "usage": {
            "input_tokens": 12,
            "output_tokens": 9
        }
    })";

    std::string adapted = adaptChatResponse(provider, anthropic_resp, "claude-3-5-sonnet");
    LR_CHECK(adapted.find(R"("object":"chat.completion")") != std::string::npos);
    LR_CHECK(adapted.find(R"("model":"claude-3-5-sonnet")") != std::string::npos);
    LR_CHECK(adapted.find("Hello! How can I assist you today?") != std::string::npos);
    LR_CHECK(adapted.find(R"("finish_reason":"stop")") != std::string::npos);
    LR_CHECK(adapted.find(R"("prompt_tokens":12)") != std::string::npos);
    LR_CHECK(adapted.find(R"("completion_tokens":9)") != std::string::npos);
    LR_CHECK(adapted.find(R"("total_tokens":21)") != std::string::npos);

    // Test tool_use in response
    std::string anthropic_tool_resp = R"({
        "id": "msg_02",
        "type": "message",
        "role": "assistant",
        "content": [
            {
                "type": "tool_use",
                "id": "toolu_01A09q90tc1qmvqvvdjsBs43",
                "name": "calc",
                "input": {"expr": "2+2"}
            }
        ],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 5, "output_tokens": 10}
    })";

    std::string adapted_tool = adaptChatResponse(provider, anthropic_tool_resp, "claude-3-5-sonnet");
    LR_CHECK(adapted_tool.find(R"("tool_calls":)") != std::string::npos);
    LR_CHECK(adapted_tool.find("toolu_01A09q90tc1qmvqvvdjsBs43") != std::string::npos);
    LR_CHECK(adapted_tool.find(R"("name":"calc")") != std::string::npos);
    LR_CHECK(adapted_tool.find(R"("finish_reason":"tool_calls")") != std::string::npos);
}

void testGeminiAdaptation() {
    LR_GROUP("Google Gemini request & response adaptation");

    ProviderConfig provider;
    provider.protocol = "gemini";

    std::string req = R"({
        "model": "gemini-1.5-pro",
        "messages": [
            {"role": "system", "content": "You are Gemini."},
            {"role": "user", "content": "Who made you?"},
            {"role": "assistant", "content": "Google."}
        ],
        "temperature": 0.2,
        "max_tokens": 1000
    })";

    std::string adapted_req = adaptChatRequest(provider, "gemini-1.5-pro", req, false);
    LR_CHECK(adapted_req.find(R"("systemInstruction":)") != std::string::npos);
    LR_CHECK(adapted_req.find("You are Gemini.") != std::string::npos);
    LR_CHECK(adapted_req.find(R"("role":"model")") != std::string::npos);
    LR_CHECK(adapted_req.find(R"("role":"user")") != std::string::npos);
    LR_CHECK(adapted_req.find(R"("maxOutputTokens":1000)") != std::string::npos);

    std::string gemini_resp = R"({
        "candidates": [
            {
                "content": {
                    "parts": [{"text": "I am a large language model trained by Google."}],
                    "role": "model"
                },
                "finishReason": "STOP"
            }
        ],
        "usageMetadata": {
            "promptTokenCount": 15,
            "candidatesTokenCount": 10,
            "totalTokenCount": 25
        }
    })";

    std::string adapted_resp = adaptChatResponse(provider, gemini_resp, "gemini-1.5-pro");
    LR_CHECK(adapted_resp.find(R"("object":"chat.completion")") != std::string::npos);
    LR_CHECK(adapted_resp.find("I am a large language model trained by Google.") != std::string::npos);
    LR_CHECK(adapted_resp.find(R"("finish_reason":"stop")") != std::string::npos);
    LR_CHECK(adapted_resp.find(R"("prompt_tokens":15)") != std::string::npos);
    LR_CHECK(adapted_resp.find(R"("completion_tokens":10)") != std::string::npos);
    LR_CHECK(adapted_resp.find(R"("total_tokens":25)") != std::string::npos);
}

void testOpenAiResponsesAdaptation() {
    LR_GROUP("OpenAI Responses API adaptation");

    std::string responses_req = R"({
        "model": "gpt-4o",
        "input": "Explain relativity in one sentence.",
        "temperature": 0.5
    })";

    std::string chat_req = adaptResponsesToChat(responses_req);
    LR_CHECK(chat_req.find(R"("messages":)") != std::string::npos);
    LR_CHECK(chat_req.find(R"("role":"user")") != std::string::npos);
    LR_CHECK(chat_req.find("Explain relativity in one sentence.") != std::string::npos);

    std::string chat_resp = R"({
        "id": "chatcmpl-999",
        "object": "chat.completion",
        "created": 1700000000,
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "Space and time are linked together."
                },
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 10,
            "completion_tokens": 8,
            "total_tokens": 18
        }
    })";

    std::string responses_resp = adaptChatToResponses(chat_resp, "gpt-4o");
    LR_CHECK(responses_resp.find(R"("object":"response")") != std::string::npos);
    LR_CHECK(responses_resp.find(R"("status":"completed")") != std::string::npos);
    LR_CHECK(responses_resp.find("Space and time are linked together.") != std::string::npos);
}

void testStreamProtocolAdapterAnthropic() {
    LR_GROUP("StreamProtocolAdapter: Anthropic SSE translation");

    ProviderConfig provider;
    provider.protocol = "anthropic";

    StreamProtocolAdapter adapter(provider, "claude-3-5-sonnet", "req_123");

    std::string chunk1 = "event: message_start\r\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_abc\"}}\r\n\r\n";
    std::string out1 = adapter.feed(chunk1);
    LR_CHECK(out1.find(R"("object":"chat.completion.chunk")") != std::string::npos);
    LR_CHECK(out1.find(R"("role":"assistant")") != std::string::npos);

    std::string chunk2 = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n";
    std::string out2 = adapter.feed(chunk2);
    LR_CHECK(out2.find(R"("content":"Hello")") != std::string::npos);

    std::string chunk3 = "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":5}}\n\n";
    std::string out3 = adapter.feed(chunk3);
    LR_CHECK(out3.find(R"("finish_reason":"stop")") != std::string::npos);

    std::string chunk4 = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    std::string out4 = adapter.feed(chunk4);
    LR_CHECK(out4.find("data: [DONE]\n\n") != std::string::npos);

    // Calling finish() after stream already cleanly terminated should not emit duplicate [DONE]
    std::string out5 = adapter.finish();
    LR_CHECK_EQ(out5, "");
}

void testStreamProtocolAdapterGemini() {
    LR_GROUP("StreamProtocolAdapter: Gemini SSE translation");

    ProviderConfig provider;
    provider.protocol = "gemini";

    StreamProtocolAdapter adapter(provider, "gemini-1.5-pro", "req_gem_1");

    std::string chunk1 = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"Hi!\"}], \"role\": \"model\"}}]}\r\n\r\n";
    std::string out1 = adapter.feed(chunk1);
    LR_CHECK(out1.find(R"("object":"chat.completion.chunk")") != std::string::npos);
    LR_CHECK(out1.find(R"("role":"assistant")") != std::string::npos);
    LR_CHECK(out1.find(R"("content":"Hi!")") != std::string::npos);

    std::string chunk2 = "data: {\"candidates\": [{\"finishReason\": \"STOP\"}]}\n\n";
    std::string out2 = adapter.feed(chunk2);
    LR_CHECK(out2.find(R"("finish_reason":"stop")") != std::string::npos);
    LR_CHECK(out2.find("data: [DONE]\n\n") != std::string::npos);

    LR_CHECK_EQ(adapter.finish(), "");
}

void testStreamProtocolAdapterOpenAiPassthrough() {
    LR_GROUP("StreamProtocolAdapter: OpenAI passthrough");

    ProviderConfig provider;
    provider.protocol = "openai";

    StreamProtocolAdapter adapter(provider, "gpt-4o", "req_passthrough");

    std::string raw = "data: {\"choices\":[{\"delta\":{\"content\":\"test\"}}]}\n\n";
    std::string out = adapter.feed(raw);
    LR_CHECK_EQ(out, raw);
    LR_CHECK_EQ(adapter.finish(), "");
}

void testReasoningMapping() {
    LR_GROUP("reasoning blocks survive a protocol conversion");

    const auto parse = [](const std::string &text) { return json::parse(text, nullptr, false); };

    // ── Anthropic upstream → Chat client ────────────────────────────────────
    {
        ProviderConfig anthropic;
        anthropic.protocol = "anthropic";
        const std::string upstream = R"({
            "id":"msg_1","type":"message","role":"assistant",
            "content":[
              {"type":"thinking","thinking":"weighing the options","signature":"sig-abc"},
              {"type":"redacted_thinking","data":"encrypted-blob"},
              {"type":"text","text":"the answer"}
            ],
            "stop_reason":"end_turn",
            "usage":{"input_tokens":10,"output_tokens":4}
        })";
        const json out = parse(adaptChatResponse(anthropic, upstream, "claude-3-5-sonnet"));
        LR_CHECK(!out.is_discarded());
        const auto &message = out["choices"][0]["message"];
        LR_CHECK_EQ(message.value("reasoning_content", std::string{}), "weighing the options");
        // The answer is still the answer: reasoning must not be folded into it.
        LR_CHECK_EQ(message.value("content", std::string{}), "the answer");
    }

    // ── Chat upstream → Anthropic client ────────────────────────────────────
    {
        const std::string upstream = R"({
            "id":"chatcmpl-1","object":"chat.completion","model":"m",
            "choices":[{"index":0,"message":{"role":"assistant",
                        "reasoning_content":"first the plan","content":"then the answer"},
                        "finish_reason":"stop"}],
            "usage":{"prompt_tokens":3,"completion_tokens":5}
        })";
        const json out = parse(adaptChatToAnthropic(upstream, "claude-3-5-sonnet"));
        LR_CHECK(!out.is_discarded());
        const auto &blocks = out["content"];
        LR_CHECK(blocks.is_array() && blocks.size() == 2);
        if (blocks.is_array() && blocks.size() == 2) {
            // Anthropic reads its blocks in order, and thinking comes first.
            LR_CHECK_EQ(blocks[0].value("type", std::string{}), "thinking");
            LR_CHECK_EQ(blocks[0].value("thinking", std::string{}), "first the plan");
            LR_CHECK_EQ(blocks[1].value("type", std::string{}), "text");
            LR_CHECK_EQ(blocks[1].value("text", std::string{}), "then the answer");
        }
        // A bare chat answer stays a single text block.
        const json plain = parse(adaptChatToAnthropic(
            R"({"id":"c","choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})", "m"));
        LR_CHECK(!plain.is_discarded());
        LR_CHECK_EQ(static_cast<long long>(plain["content"].size()), 1);
        LR_CHECK_EQ(plain["content"][0].value("type", std::string{}), "text");
    }

    // ── Anthropic request → Chat upstream ───────────────────────────────────
    {
        const std::string request = R"({
            "model":"m","max_tokens":100,
            "messages":[{"role":"user","content":"q"},
                        {"role":"assistant","content":[
                            {"type":"thinking","thinking":"recall previous step","signature":"sig"},
                            {"type":"text","text":"previous answer"}
                        ]},
                        {"role":"user","content":"follow up"}]
        })";
        const json out = parse(adaptAnthropicToChat(request));
        LR_CHECK(!out.is_discarded());
        const auto &messages = out["messages"];
        LR_CHECK(messages.is_array());
        bool found = false;
        for (const auto &message : messages) {
            if (message.value("role", std::string{}) == "assistant") {
                found = true;
                LR_CHECK_EQ(message.value("reasoning_content", std::string{}), "recall previous step");
                LR_CHECK_EQ(message.value("content", std::string{}), "previous answer");
            }
        }
        LR_CHECK_MSG(found, "the assistant turn went missing");
    }

    // ── Gemini upstream → Chat client ───────────────────────────────────────
    {
        ProviderConfig gemini;
        gemini.protocol = "gemini";
        const std::string upstream = R"({
            "candidates":[{"content":{"role":"model","parts":[
                {"text":"thinking out loud","thought":true},
                {"text":"the answer"}
            ]},"finishReason":"STOP"}],
            "usageMetadata":{"promptTokenCount":7,"candidatesTokenCount":3}
        })";
        const json out = parse(adaptChatResponse(gemini, upstream, "gemini-2.0-flash"));
        LR_CHECK(!out.is_discarded());
        const auto &message = out["choices"][0]["message"];
        LR_CHECK_EQ(message.value("reasoning_content", std::string{}), "thinking out loud");
        // This is the bug the mapping fixes: a `thought` part used to be appended
        // to the content, so the caller was shown the model's reasoning as if it
        // were the reply.
        LR_CHECK_EQ(message.value("content", std::string{}), "the answer");
    }

    // ── Responses upstream → Chat client ────────────────────────────────────
    {
        ProviderConfig responses;
        responses.protocol = "openai_responses";
        const std::string upstream = R"({
            "id":"resp_1","object":"response","status":"completed",
            "output":[
              {"id":"rs_1","type":"reasoning",
               "summary":[{"type":"summary_text","text":"weighed it"}]},
              {"id":"msg_1","type":"message","role":"assistant",
               "content":[{"type":"text","text":"the answer"}]}
            ],
            "usage":{"input_tokens":9,"output_tokens":4}
        })";
        const json out = parse(adaptChatResponse(responses, upstream, "gpt-5"));
        LR_CHECK(!out.is_discarded());
        const auto &message = out["choices"][0]["message"];
        LR_CHECK_EQ(message.value("reasoning_content", std::string{}), "weighed it");
        LR_CHECK_EQ(message.value("content", std::string{}), "the answer");
    }

    // ── Chat upstream → Responses client ────────────────────────────────────
    {
        const json out = parse(adaptChatToResponses(
            R"({"id":"c","object":"chat.completion","choices":[{"message":
                 {"role":"assistant","reasoning_content":"planned it","content":"done"}}]})",
            "gpt-5"));
        LR_CHECK(!out.is_discarded());
        const auto &output = out["output"];
        LR_CHECK(output.is_array() && output.size() == 2);
        if (output.is_array() && output.size() == 2) {
            // Reasoning first, which is the order a Responses client renders.
            LR_CHECK_EQ(output[0].value("type", std::string{}), "reasoning");
            LR_CHECK_EQ(output[0]["summary"][0].value("text", std::string{}), "planned it");
            LR_CHECK_EQ(output[1].value("type", std::string{}), "message");
            LR_CHECK_EQ(output[1]["content"][0].value("text", std::string{}), "done");
        }
    }

    // ── Chat upstream → Gemini client ───────────────────────────────────────
    {
        const json out = parse(adaptChatToGemini(
            R"({"id":"c","choices":[{"message":{"role":"assistant",
                 "reasoning_content":"hmm","content":"ok"},"finish_reason":"stop"}]})",
            "gemini-2.0-flash"));
        LR_CHECK(!out.is_discarded());
        const auto &parts = out["candidates"][0]["content"]["parts"];
        LR_CHECK(parts.is_array() && parts.size() == 2);
        if (parts.is_array() && parts.size() == 2) {
            LR_CHECK_EQ(parts[0].value("text", std::string{}), "hmm");
            LR_CHECK_EQ(parts[0].value("thought", false), true);
            LR_CHECK_EQ(parts[1].value("text", std::string{}), "ok");
            LR_CHECK_EQ(parts[1].value("thought", false), false);
        }
    }

    // ── Responses streaming, both ways ──────────────────────────────────────
    {
        // An OpenAI relay answering a client that asked /v1/responses with
        // stream: true. Without a Responses emission branch the adapter built a
        // stream and threw it away, so the client got an empty body.
        StreamProtocolAdapter adapter("openai", "openai_responses", "gpt-5", "req_resp");
        const std::string text = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n");
        LR_CHECK_MSG(text.find("response.output_text.delta") != std::string::npos,
                     "no Responses delta event was emitted: " + text);
        LR_CHECK(text.find("\"delta\":\"hi\"") != std::string::npos);
        const std::string reasoning = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"reasoning_content\":\"why\"}}]}\n\n");
        LR_CHECK(reasoning.find("response.reasoning_summary_text.delta") != std::string::npos);
        const std::string fin = adapter.finish();
        LR_CHECK_MSG(fin.find("response.completed") != std::string::npos,
                     "the Responses stream never completed: " + fin);
    }
    {
        // And the other way: a Responses relay's events, read by the adapter.
        StreamProtocolAdapter adapter("openai_responses", "openai", "gpt-5", "req_resp2");
        adapter.feed("event: response.created\ndata: {\"type\":\"response.created\","
                     "\"response\":{\"id\":\"resp_9\"}}\n\n");
        const std::string text = adapter.feed(
            "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\","
            "\"delta\":\"hi\"}\n\n");
        LR_CHECK_MSG(text.find("\"content\":\"hi\"") != std::string::npos,
                     "a Responses text delta did not reach the client: " + text);
        const std::string reasoning = adapter.feed(
            "event: response.reasoning_summary_text.delta\ndata: "
            "{\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"why\"}\n\n");
        LR_CHECK_MSG(reasoning.find("\"reasoning_content\":\"why\"") != std::string::npos,
                     "a Responses reasoning delta did not reach the client: " + reasoning);
        const std::string done = adapter.feed(
            "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":"
            "{\"usage\":{\"input_tokens\":5,\"output_tokens\":2}}}\n\n");
        LR_CHECK(done.find("[DONE]") != std::string::npos);
    }

    // ── Streaming: Anthropic thinking deltas reach an OpenAI client ─────────
    {
        StreamProtocolAdapter adapter("anthropic", "openai", "claude-3-5-sonnet", "req_reason");
        const std::string started = adapter.feed(
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_2\","
            "\"usage\":{\"input_tokens\":5}}}\n\n");
        LR_CHECK(started.find("message_start") == std::string::npos); // converted, not passed through
        const std::string thinking = adapter.feed(
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"step one\"}}\n\n");
        LR_CHECK_MSG(thinking.find("\"reasoning_content\":\"step one\"") != std::string::npos,
                     "a thinking delta did not reach the client as reasoning: " + thinking);
        const std::string text = adapter.feed(
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"answer\"}}\n\n");
        LR_CHECK(text.find("\"content\":\"answer\"") != std::string::npos);
    }

    // ── Streaming: an OpenAI relay's reasoning becomes an Anthropic thinking
    //    block, in the order Anthropic requires ────────────────────────────────
    {
        StreamProtocolAdapter adapter("openai", "anthropic", "gpt-4o", "req_reverse");
        const std::string reasoning = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"reasoning_content\":\"hidden\"}}]}\n\n");
        LR_CHECK_MSG(reasoning.find("event: message_start") != std::string::npos, reasoning);
        LR_CHECK_MSG(reasoning.find("\"type\":\"thinking\"") != std::string::npos,
                     "reasoning did not open a thinking block: " + reasoning);
        LR_CHECK_MSG(reasoning.find("\"type\":\"thinking_delta\"") != std::string::npos,
                     "reasoning did not arrive as a thinking delta: " + reasoning);
        LR_CHECK_MSG(reasoning.find("\"index\":0") != std::string::npos,
                     "the thinking block must be index 0: " + reasoning);
        // The text block must not open while the thinking block is still open:
        // Anthropic wants thinking first, and interleaving them is the block
        // sequence a strict client rejects.
        LR_CHECK_MSG(reasoning.find("\"type\":\"text\"") == std::string::npos,
                     "the text block opened before the thinking block closed: " + reasoning);

        const std::string text = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n");
        LR_CHECK_MSG(text.find("\"type\":\"content_block_stop\"") != std::string::npos, text);
        LR_CHECK_MSG(text.find("\"type\":\"text\"") != std::string::npos, text);
        LR_CHECK_MSG(text.find("\"index\":1") != std::string::npos,
                     "the text block must take the next index: " + text);
        LR_CHECK_MSG(text.find("\"type\":\"text_delta\"") != std::string::npos, text);
        LR_CHECK_MSG(text.find("answer") != std::string::npos, text);

        const std::string done = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n");
        LR_CHECK_MSG(done.find("message_stop") != std::string::npos, done);
        LR_CHECK_MSG(done.find("end_turn") != std::string::npos, done);
        LR_CHECK_EQ(adapter.finish(), "");
    }

    // No reasoning at all keeps the shape this adapter always produced: one text
    // block at index 0, opened and closed.
    {
        StreamProtocolAdapter adapter("openai", "anthropic", "gpt-4o", "req_plain");
        const std::string out = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n");
        LR_CHECK_MSG(out.find("\"type\":\"text\"") != std::string::npos, out);
        LR_CHECK_MSG(out.find("thinking") == std::string::npos,
                     "no reasoning means no thinking block: " + out);
        LR_CHECK_MSG(out.find("\"index\":0") != std::string::npos, out);
        LR_CHECK_MSG(out.find("content_block_stop") != std::string::npos, out);
        LR_CHECK_MSG(out.find("message_stop") != std::string::npos, out);
    }

    // A stream cut short before any finish_reason still closes the block it
    // opened, at the index it actually used — not at a hard-coded zero.
    {
        StreamProtocolAdapter adapter("openai", "anthropic", "gpt-4o", "req_cut");
        adapter.feed("data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"reasoning_content\":\"r\"}}]}\n\n");
        adapter.feed("data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"content\":\"t\"}}]}\n\n");
        const std::string fin = adapter.finish();
        LR_CHECK_MSG(fin.find("\"index\":1") != std::string::npos,
                     "the open text block is index 1 and must be closed there: " + fin);
        LR_CHECK_MSG(fin.find("message_stop") != std::string::npos, fin);
    }
}

void testStreamUsageObserver() {
    LR_GROUP("StreamUsageObserver reads the counts each protocol hides in its tail");
    using literouter::StreamUsageObserver;
    using literouter::TokenUsage;
    // A predicate rather than a pair the assertions compare: a comma (or a
    // braced initializer) inside a macro argument is a second argument, and both
    // would have to be parenthesized at every call site.
    const auto isCounts = [](const StreamUsageObserver &observer, std::uint64_t prompt,
                             std::uint64_t completion) {
        const TokenUsage usage = observer.usage();
        return usage.prompt == prompt && usage.completion == completion;
    };

    {
        // OpenAI, when the client asked for stream_options.include_usage: one
        // final chunk carries both numbers, after the deltas.
        StreamUsageObserver observer;
        observer.feed("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n");
        LR_CHECK(isCounts(observer, 0, 0));
        observer.feed("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7}}\n\n");
        LR_CHECK(isCounts(observer, 11, 7));
        observer.feed("data: [DONE]\n\n");
        LR_CHECK(isCounts(observer, 11, 7));
    }
    {
        // The chunk boundary is not obliged to fall on an event boundary — it
        // can even fall inside the word "usage".
        StreamUsageObserver observer;
        observer.feed("data: {\"choices\":[],\"usa");
        LR_CHECK(isCounts(observer, 0, 0));
        observer.feed("ge\":{\"prompt_tokens\":3,\"completion_tokens\":4}}\n\n");
        LR_CHECK(isCounts(observer, 3, 4));
    }
    {
        // Anthropic splits the pair: input_tokens in message_start, the final
        // output_tokens in message_delta.
        StreamUsageObserver observer;
        observer.feed("event: message_start\ndata: {\"type\":\"message_start\","
                      "\"message\":{\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n");
        LR_CHECK(isCounts(observer, 25, 1));
        observer.feed("event: message_delta\ndata: {\"type\":\"message_delta\","
                      "\"usage\":{\"output_tokens\":42}}\n\n");
        // The larger of the two wins: a cumulative count must not be added to
        // the partial one it grew out of.
        LR_CHECK(isCounts(observer, 25, 42));
    }
    {
        // Gemini reports a cumulative usageMetadata on every chunk.
        StreamUsageObserver observer;
        observer.feed("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"a\"}]}}],"
                      "\"usageMetadata\":{\"promptTokenCount\":9,\"candidatesTokenCount\":2}}\n\n");
        observer.feed("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"b\"}]}}],"
                      "\"usageMetadata\":{\"promptTokenCount\":9,\"candidatesTokenCount\":6}}\n\n");
        LR_CHECK(isCounts(observer, 9, 6));
    }
    {
        // Without ?alt=sse Gemini answers with an array of the same objects.
        StreamUsageObserver observer;
        observer.feed("[{\"usageMetadata\":{\"promptTokenCount\":4,\"candidatesTokenCount\":5}}]\n");
        LR_CHECK(isCounts(observer, 4, 5));
    }
    {
        // OpenAI Responses nests the pair under response.usage.
        StreamUsageObserver observer;
        observer.feed("event: response.completed\ndata: {\"type\":\"response.completed\","
                      "\"response\":{\"usage\":{\"input_tokens\":8,\"output_tokens\":13}}}\n\n");
        LR_CHECK(isCounts(observer, 8, 13));
    }
    {
        // The fast path: a chunk that ends on an event boundary and mentions no
        // usage is left alone entirely — nothing carried, nothing parsed — and
        // the counts still arrive from a later chunk.
        StreamUsageObserver observer;
        observer.feed("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n");
        observer.feed("data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n");
        LR_CHECK(isCounts(observer, 0, 0));
        observer.feed("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":6,\"completion_tokens\":2}}\n\n");
        LR_CHECK(isCounts(observer, 6, 2));
    }
    {
        // A relay that never names usage is not guessed at, and neither the
        // literal "usage" in a delta nor a broken event is mistaken for one.
        StreamUsageObserver observer;
        observer.feed("data: {\"choices\":[{\"delta\":{\"content\":\"usage of words\"}}]}\n\n");
        observer.feed("data: {\"usage\":not-json}\n\n");
        observer.feed("[DONE]\n");
        LR_CHECK(isCounts(observer, 0, 0));
    }
    {
        // Counts sent as strings, or negative, are left alone rather than
        // read as something they are not.
        StreamUsageObserver observer;
        observer.feed("data: {\"usage\":{\"prompt_tokens\":\"many\",\"completion_tokens\":-3}}\n\n");
        LR_CHECK(isCounts(observer, 0, 0));
    }
}

void testAnthropicIngressAdaptation() {
    LR_GROUP("Anthropic ingress adaptation (Anthropic -> Chat -> Anthropic)");

    std::string anthropic_req = R"({
        "model": "claude-3-5-sonnet",
        "system": "You are a helpful coding assistant.",
        "messages": [
            {"role": "user", "content": "Write a fizzbuzz in C++"}
        ],
        "max_tokens": 2048,
        "temperature": 0.5
    })";

    std::string canonical_chat = adaptAnthropicToChat(anthropic_req);
    LR_CHECK(canonical_chat.find(R"("model":"claude-3-5-sonnet")") != std::string::npos);
    LR_CHECK(canonical_chat.find(R"("role":"system")") != std::string::npos);
    LR_CHECK(canonical_chat.find("You are a helpful coding assistant.") != std::string::npos);
    LR_CHECK(canonical_chat.find(R"("role":"user")") != std::string::npos);
    LR_CHECK(canonical_chat.find("Write a fizzbuzz in C++") != std::string::npos);
    LR_CHECK(canonical_chat.find(R"("max_tokens":2048)") != std::string::npos);

    // Test tool results and tool use
    std::string anthropic_tool_req = R"({
        "model": "claude-3-5-sonnet",
        "messages": [
            {
                "role": "assistant",
                "content": [
                    {"type": "text", "text": "Calling tool"},
                    {"type": "tool_use", "id": "call_123", "name": "weather", "input": {"city": "Tokyo"}}
                ]
            },
            {
                "role": "user",
                "content": [
                    {"type": "tool_result", "tool_use_id": "call_123", "content": "Sunny 22C"}
                ]
            }
        ]
    })";

    std::string chat_with_tools = adaptAnthropicToChat(anthropic_tool_req);
    LR_CHECK(chat_with_tools.find(R"("role":"assistant")") != std::string::npos);
    LR_CHECK(chat_with_tools.find(R"("tool_calls":)") != std::string::npos);
    LR_CHECK(chat_with_tools.find("call_123") != std::string::npos);
    LR_CHECK(chat_with_tools.find(R"("role":"tool")") != std::string::npos);
    LR_CHECK(chat_with_tools.find("Sunny 22C") != std::string::npos);

    // Test adapting Chat Completion response to Anthropic Message response
    std::string chat_resp = R"({
        "id": "chatcmpl-test456",
        "object": "chat.completion",
        "created": 1700000000,
        "model": "claude-3-5-sonnet",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "Tokyo is sunny today."
                },
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 15,
            "completion_tokens": 6,
            "total_tokens": 21
        }
    })";

    std::string anthropic_resp = adaptChatToAnthropic(chat_resp, "claude-3-5-sonnet");
    LR_CHECK(anthropic_resp.find(R"("type":"message")") != std::string::npos);
    LR_CHECK(anthropic_resp.find(R"("role":"assistant")") != std::string::npos);
    LR_CHECK(anthropic_resp.find(R"("stop_reason":"end_turn")") != std::string::npos);
    LR_CHECK(anthropic_resp.find("Tokyo is sunny today.") != std::string::npos);
    LR_CHECK(anthropic_resp.find(R"("input_tokens":15)") != std::string::npos);
    LR_CHECK(anthropic_resp.find(R"("output_tokens":6)") != std::string::npos);
}

void testGeminiIngressAdaptation() {
    LR_GROUP("Gemini ingress adaptation (Gemini -> Chat -> Gemini)");

    std::string gemini_req = R"({
        "systemInstruction": {
            "parts": [{"text": "You are Gemini."}]
        },
        "contents": [
            {
                "role": "user",
                "parts": [{"text": "What is gravity?"}]
            }
        ],
        "generationConfig": {
            "temperature": 0.3,
            "maxOutputTokens": 500
        }
    })";

    std::string chat_req = adaptGeminiToChat(gemini_req, "gemini-1.5-pro");
    LR_CHECK(chat_req.find(R"("model":"gemini-1.5-pro")") != std::string::npos);
    LR_CHECK(chat_req.find(R"("role":"system")") != std::string::npos);
    LR_CHECK(chat_req.find("You are Gemini.") != std::string::npos);
    LR_CHECK(chat_req.find(R"("role":"user")") != std::string::npos);
    LR_CHECK(chat_req.find("What is gravity?") != std::string::npos);
    LR_CHECK(chat_req.find(R"("max_tokens":500)") != std::string::npos);

    // Test adapting Chat Completion response to Gemini response
    std::string chat_resp = R"({
        "id": "chatcmpl-gemini-test",
        "object": "chat.completion",
        "created": 1700000000,
        "model": "gemini-1.5-pro",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "Gravity is a fundamental interaction."
                },
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 12,
            "completion_tokens": 7,
            "total_tokens": 19
        }
    })";

    std::string gemini_resp = adaptChatToGemini(chat_resp, "gemini-1.5-pro");
    LR_CHECK(gemini_resp.find(R"("candidates":)") != std::string::npos);
    LR_CHECK(gemini_resp.find(R"("role":"model")") != std::string::npos);
    LR_CHECK(gemini_resp.find("Gravity is a fundamental interaction.") != std::string::npos);
    LR_CHECK(gemini_resp.find(R"("finishReason":"STOP")") != std::string::npos);
    LR_CHECK(gemini_resp.find(R"("promptTokenCount":12)") != std::string::npos);
    LR_CHECK(gemini_resp.find(R"("candidatesTokenCount":7)") != std::string::npos);
}

void testMultiProtocolStreaming() {
    LR_GROUP("StreamProtocolAdapter: multi-protocol streaming and passthrough");

    // 1. Anthropic -> Anthropic passthrough
    {
        StreamProtocolAdapter adapter("anthropic", "anthropic", "claude-3-5-sonnet", "req_1");
        std::string raw = "event: message_delta\ndata: {\"delta\":{\"text\":\"hello\"}}\n\n";
        LR_CHECK_EQ(adapter.feed(raw), raw);
        LR_CHECK_EQ(adapter.finish(), "");
    }

    // 2. Gemini -> Gemini passthrough
    {
        StreamProtocolAdapter adapter("gemini", "gemini", "gemini-1.5-pro", "req_2");
        std::string raw = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini\"}]}}]}\n\n";
        LR_CHECK_EQ(adapter.feed(raw), raw);
        LR_CHECK_EQ(adapter.finish(), "");
    }

    // 3. OpenAI -> Anthropic
    {
        StreamProtocolAdapter adapter("openai", "anthropic", "claude-3-5-sonnet", "req_oa_to_anth");
        std::string chunk1 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello Anthropic\"},\"finish_reason\":null}]}\n\n";
        std::string out1 = adapter.feed(chunk1);
        LR_CHECK(out1.find("event: message_start") != std::string::npos);
        LR_CHECK(out1.find("event: content_block_delta") != std::string::npos);
        LR_CHECK(out1.find("Hello Anthropic") != std::string::npos);

        std::string chunk2 = "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
        std::string out2 = adapter.feed(chunk2);
        LR_CHECK(out2.find("event: message_delta") != std::string::npos);
        LR_CHECK(out2.find("event: message_stop") != std::string::npos);
        LR_CHECK(out2.find("end_turn") != std::string::npos);

        LR_CHECK_EQ(adapter.finish(), "");
    }

    // 4. OpenAI -> Gemini
    {
        StreamProtocolAdapter adapter("openai", "gemini", "gemini-1.5-pro", "req_oa_to_gem");
        std::string chunk1 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello Gemini\"},\"finish_reason\":null}]}\n\n";
        std::string out1 = adapter.feed(chunk1);
        LR_CHECK(out1.find(R"("role":"model")") != std::string::npos);
        LR_CHECK(out1.find("Hello Gemini") != std::string::npos);

        std::string chunk2 = "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
        std::string out2 = adapter.feed(chunk2);
        LR_CHECK(out2.find(R"("finishReason":"STOP")") != std::string::npos);

        LR_CHECK_EQ(adapter.finish(), "");
    }
}

} // namespace

namespace {

// ── the protocols added alongside Azure, Vertex, Bedrock and Ollama ──────────

// A CRC32 of the standard check string, pinned so this file's own framer (used
// to build Bedrock test frames) is provably the CRC32 the parser expects rather
// than two implementations of the same mistake.
std::uint32_t crc32Of(const std::string &data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data) {
        crc ^= static_cast<unsigned char>(ch);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1) - 1)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void appendU32(std::string &out, std::uint32_t value) {
    out += static_cast<char>((value >> 24) & 0xFF);
    out += static_cast<char>((value >> 16) & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>(value & 0xFF);
}

// One AWS event-stream frame, built the way the service builds them: a
// length-prefixed prelude with its own CRC, typed headers, the payload, and a
// CRC over everything before it.
std::string bedrockFrame(const std::string &event_type, const std::string &payload) {
    const auto stringHeader = [](const std::string &name, const std::string &value) {
        std::string out;
        out += static_cast<char>(name.size());
        out += name;
        out += static_cast<char>(7); // type 7 = string
        const auto length = static_cast<std::uint16_t>(value.size());
        out += static_cast<char>((length >> 8) & 0xFF);
        out += static_cast<char>(length & 0xFF);
        out += value;
        return out;
    };
    std::string headers = stringHeader(":message-type", "event");
    headers += stringHeader(":event-type", event_type);
    headers += stringHeader(":content-type", "application/json");

    const auto total = static_cast<std::uint32_t>(16 + headers.size() + payload.size());
    std::string prelude;
    appendU32(prelude, total);
    appendU32(prelude, static_cast<std::uint32_t>(headers.size()));
    appendU32(prelude, crc32Of(prelude));

    std::string message = prelude + headers + payload;
    appendU32(message, crc32Of(message));
    return message;
}

void testNewOutboundPaths() {
    LR_GROUP("azure, vertex, ollama and bedrock resolve their own paths");
    // A named predicate because the assertion helpers overload on string and
    // integer only, and an enum would silently pick the integer one.
    const auto shapeIs = [](std::string_view protocol, literouter::WireShape expected,
                            std::string_view what) {
        LR_CHECK_MSG(literouter::wireShapeOf(protocol) == expected, std::string{what});
    };

    {
        ProviderConfig azure;
        azure.protocol = "azure";
        azure.base_url = "https://demo.openai.azure.com";
        // The model name is a *deployment* in the path, and the api-version is
        // mandatory — the two reasons Azure cannot share OpenAI's path.
        LR_CHECK_EQ(resolveChatPath(azure, "gpt-4o", false),
                    "/openai/deployments/gpt-4o/chat/completions?api-version=2024-10-21");
        azure.api_version = "2024-08-01-preview";
        LR_CHECK_EQ(resolveChatPath(azure, "my-deployment", false),
                    "/openai/deployments/my-deployment/chat/completions?api-version=2024-08-01-preview");
        // An explicit chat_path still wins, for a gateway in front of Azure.
        azure.chat_path = "/custom";
        LR_CHECK_EQ(resolveChatPath(azure, "gpt-4o", false), "/custom");
        // Azure speaks OpenAI's JSON, so a body needs no conversion.
        shapeIs("azure", literouter::WireShape::OpenAi, "azure shape");
    }
    {
        ProviderConfig vertex;
        vertex.protocol = "vertex";
        vertex.project = "demo";
        vertex.region = "europe-west4";
        LR_CHECK_EQ(resolveChatPath(vertex, "gemini-2.0-flash", false),
                    "/v1/projects/demo/locations/europe-west4/publishers/google/models/"
                    "gemini-2.0-flash:generateContent");
        LR_CHECK_EQ(resolveChatPath(vertex, "gemini-2.0-flash", true),
                    "/v1/projects/demo/locations/europe-west4/publishers/google/models/"
                    "gemini-2.0-flash:streamGenerateContent?alt=sse");
        vertex.api_version = "v1beta";
        LR_CHECK(resolveChatPath(vertex, "m", false).starts_with("/v1beta/projects/demo"));
        LR_CHECK_EQ(resolveModelsPath(vertex),
                    "/v1beta/projects/demo/locations/europe-west4/publishers/google/models");
        // Vertex is Gemini's JSON behind a different path and a bearer token.
        shapeIs("vertex", literouter::WireShape::Gemini, "vertex shape");
        const json out = json::parse(
            adaptChatRequest(vertex, "gemini-2.0-flash",
                             R"({"messages":[{"role":"user","content":"hi"}],"temperature":0.5})",
                             false),
            nullptr, false);
        LR_CHECK(!out.is_discarded());
        LR_CHECK(out.contains("contents"));
        LR_CHECK(out.contains("generationConfig"));
    }
    {
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        LR_CHECK_EQ(resolveChatPath(ollama, "llama3", false), "/api/chat");
        LR_CHECK_EQ(resolveModelsPath(ollama), "/api/tags");
        shapeIs("ollama", literouter::WireShape::Ollama, "ollama shape");
    }
    {
        ProviderConfig bedrock;
        bedrock.protocol = "bedrock";
        // A Bedrock model id contains a colon, which is why the SigV4 path has
        // to be percent-encoded rather than signed verbatim.
        LR_CHECK_EQ(resolveChatPath(bedrock, "anthropic.claude-3-5-sonnet-20241022-v2:0", false),
                    "/model/anthropic.claude-3-5-sonnet-20241022-v2:0/converse");
        LR_CHECK_EQ(resolveChatPath(bedrock, "m", true), "/model/m/converse-stream");
        shapeIs("bedrock", literouter::WireShape::Bedrock, "bedrock shape");
    }
    // Every protocol name maps to a shape, and the aliases still mean OpenAI.
    shapeIs("openai_compatible", literouter::WireShape::OpenAi, "openai_compatible shape");
    shapeIs("openai_chat", literouter::WireShape::OpenAi, "openai_chat shape");
    shapeIs("openai", literouter::WireShape::OpenAi, "openai shape");
    shapeIs("", literouter::WireShape::OpenAi, " shape");
    shapeIs("ANTHROPIC", literouter::WireShape::Anthropic, "ANTHROPIC shape");
    shapeIs("openai_responses", literouter::WireShape::Responses, "openai_responses shape");
}

void testOllamaAdaptation() {
    LR_GROUP("Ollama's /api/chat body and reply are translated both ways");

    {
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const json req = json::parse(
            adaptChatRequest(ollama, "llama3",
                             R"({"messages":[{"role":"system","content":"be brief"},
                                  {"role":"user","content":[{"type":"text","text":"hello"}]}],
                                 "temperature":0.2,"max_tokens":64,"stop":["x"]})",
                             false),
            nullptr, false);
        LR_CHECK(!req.is_discarded());
        LR_CHECK_EQ(req["model"].get<std::string>(), "llama3");
        LR_CHECK_EQ(req["stream"].get<bool>(), false);
        LR_CHECK_EQ(static_cast<long long>(req["messages"].size()), 2);
        // An array of parts is folded into the single string Ollama wants.
        LR_CHECK_EQ(req["messages"][1]["content"].get<std::string>(), "hello");
        LR_CHECK_EQ(req["options"]["num_predict"], 64);
        LR_CHECK_MSG(std::abs(req["options"]["temperature"].get<double>() - 0.2) < 1e-9,
                     "temperature must survive as a double, not be truncated");
        LR_CHECK(req["options"].contains("stop"));
        LR_CHECK_MSG(!req["options"].contains("max_tokens"),
                     "Ollama does not know OpenAI's knob name");
    }
    {
        // A data-URL image becomes the bare base64 string Ollama takes, and a
        // `developer` message becomes `system` because Ollama has no such role.
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const json req = json::parse(
            adaptChatRequest(ollama, "llava",
                             R"({"messages":[{"role":"developer","content":"d"},
                                  {"role":"user","content":[{"type":"text","text":"what is this"},
                                   {"type":"image_url","image_url":{"url":"data:image/png;base64,QUJD"}}]}]})",
                             false),
            nullptr, false);
        LR_CHECK(!req.is_discarded());
        LR_CHECK_EQ(req["messages"][0]["role"].get<std::string>(), "system");
        LR_CHECK_EQ(req["messages"][1]["images"][0].get<std::string>(), "QUJD");
    }
    {
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const json out = json::parse(
            adaptChatResponse(ollama,
                              R"({"model":"llama3","message":{"role":"assistant","content":"hi there","thinking":"hmm"},
                                  "done":true,"done_reason":"stop","prompt_eval_count":7,"eval_count":3})",
                              "llama3"),
            nullptr, false);
        LR_CHECK(!out.is_discarded());
        LR_CHECK_EQ(out["choices"][0]["message"]["content"].get<std::string>(), "hi there");
        LR_CHECK_EQ(out["choices"][0]["message"]["reasoning_content"].get<std::string>(), "hmm");
        LR_CHECK_EQ(out["choices"][0]["finish_reason"].get<std::string>(), "stop");
        LR_CHECK_EQ(out["usage"]["prompt_tokens"], 7);
        LR_CHECK_EQ(out["usage"]["completion_tokens"], 3);
        LR_CHECK_EQ(out["usage"]["total_tokens"], 10);
        // The counts are what makes a streamed answer billable rather than free.
        LR_CHECK(literouter::tokenUsageReported(out.dump()));
    }
    {
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const json out = json::parse(
            adaptChatResponse(ollama,
                              R"({"message":{"content":"","tool_calls":[{"function":{"name":"f","arguments":{"a":1}}}]},"done":true})",
                              "llama3"),
            nullptr, false);
        LR_CHECK_EQ(out["choices"][0]["message"]["tool_calls"][0]["function"]["name"].get<std::string>(), "f");
        LR_CHECK_EQ(out["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"].get<std::string>(),
                    R"({"a":1})");
        LR_CHECK_EQ(out["choices"][0]["finish_reason"].get<std::string>(), "tool_calls");
    }
    {
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const json out = json::parse(
            adaptChatResponse(ollama, R"({"message":{"content":"cut"},"done":true,"done_reason":"length"})",
                              "llama3"),
            nullptr, false);
        LR_CHECK_EQ(out["choices"][0]["finish_reason"].get<std::string>(), "length");
    }
}

void testBedrockAdaptation() {
    LR_GROUP("Bedrock's Converse body and reply are translated both ways");

    {
        ProviderConfig bedrock;
        bedrock.protocol = "bedrock";
        const json req = json::parse(
            adaptChatRequest(bedrock, "m",
                             R"({"messages":[{"role":"system","content":"sys"},
                                  {"role":"user","content":"hi"},
                                  {"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function",
                                    "function":{"name":"f","arguments":"{\"a\":1}"}}]},
                                  {"role":"tool","tool_call_id":"call_1","content":"result"}],
                                 "max_tokens":128,"temperature":0.1,"stop":["z"],
                                 "tools":[{"type":"function","function":{"name":"f","description":"d",
                                   "parameters":{"type":"object"}}}]})",
                             false),
            nullptr, false);
        LR_CHECK(!req.is_discarded());
        // The system prompt is its own top-level list, not a message.
        LR_CHECK_EQ(req["system"][0]["text"].get<std::string>(), "sys");
        LR_CHECK_EQ(static_cast<long long>(req["messages"].size()), 3);
        LR_CHECK(req["messages"][1]["content"][0].contains("toolUse"));
        LR_CHECK(req["messages"][1]["content"][0]["toolUse"]["input"].contains("a"));
        LR_CHECK(req["messages"][2]["content"][0].contains("toolResult"));
        LR_CHECK_EQ(req["inferenceConfig"]["maxTokens"], 128);
        LR_CHECK(req["inferenceConfig"].contains("stopSequences"));
        LR_CHECK_EQ(req["toolConfig"]["tools"][0]["toolSpec"]["name"].get<std::string>(), "f");
        LR_CHECK(req["toolConfig"]["tools"][0]["toolSpec"]["inputSchema"].contains("json"));
        // A message with no content at all still gets a block: Converse rejects
        // an empty content list.
        const json empty = json::parse(
            adaptChatRequest(bedrock, "m", R"({"messages":[{"role":"user","content":""}]})", false),
            nullptr, false);
        LR_CHECK(!empty.is_discarded());
        LR_CHECK(empty["messages"][0]["content"].is_array());
        LR_CHECK(!empty["messages"][0]["content"].empty());
    }
    {
        ProviderConfig bedrock;
        bedrock.protocol = "bedrock";
        const json out = json::parse(
            adaptChatResponse(bedrock,
                              R"({"output":{"message":{"role":"assistant","content":[
                                    {"text":"hello"},
                                    {"reasoningContent":{"reasoningText":{"text":"because"}}},
                                    {"toolUse":{"toolUseId":"tu_1","name":"f","input":{"a":1}}}]}},
                                  "stopReason":"tool_use","usage":{"inputTokens":5,"outputTokens":2}})",
                              "m"),
            nullptr, false);
        LR_CHECK(!out.is_discarded());
        LR_CHECK_EQ(out["choices"][0]["message"]["content"].get<std::string>(), "hello");
        LR_CHECK_EQ(out["choices"][0]["message"]["reasoning_content"].get<std::string>(), "because");
        LR_CHECK_EQ(out["choices"][0]["message"]["tool_calls"][0]["function"]["name"].get<std::string>(), "f");
        LR_CHECK_EQ(out["choices"][0]["message"]["tool_calls"][0]["id"].get<std::string>(), "tu_1");
        LR_CHECK_EQ(out["choices"][0]["finish_reason"].get<std::string>(), "tool_calls");
        LR_CHECK_EQ(out["usage"]["prompt_tokens"], 5);
        LR_CHECK_EQ(out["usage"]["completion_tokens"], 2);
    }
    {
        ProviderConfig bedrock;
        bedrock.protocol = "bedrock";
        const json out = json::parse(
            adaptChatResponse(bedrock,
                              R"({"output":{"message":{"role":"assistant","content":[{"text":"cut"}]}},
                                  "stopReason":"max_tokens","usage":{"inputTokens":1,"outputTokens":9}})",
                              "m"),
            nullptr, false);
        LR_CHECK_EQ(out["choices"][0]["finish_reason"].get<std::string>(), "length");
    }
}

void testOllamaBedrockStreaming() {
    LR_GROUP("Ollama NDJSON and Bedrock event streams become OpenAI SSE");

    // Ollama streams newline-delimited JSON: no `data:` prefix, no blank-line
    // terminator. Reusing the SSE splitter would buffer the whole answer.
    {
        StreamProtocolAdapter adapter("ollama", "openai", "llama3", "req_ollama");
        const std::string out = adapter.feed(
            "{\"message\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"done\":false}\n"
            "{\"message\":{\"content\":\"lo\"},\"done\":false}\n");
        LR_CHECK_MSG(out.find("Hel") != std::string::npos, out);
        LR_CHECK_MSG(out.find("lo") != std::string::npos, out);
        LR_CHECK_MSG(out.find("chat.completion.chunk") != std::string::npos, out);
        const std::string done = adapter.feed(
            "{\"message\":{\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\","
            "\"prompt_eval_count\":4,\"eval_count\":2}\n");
        LR_CHECK_MSG(done.find("[DONE]") != std::string::npos, done);
        LR_CHECK_MSG(done.find("\"finish_reason\":\"stop\"") != std::string::npos, done);
        LR_CHECK_EQ(adapter.finish(), "");
    }
    // A chunk boundary inside a line must neither emit a partial object nor lose
    // it.
    {
        StreamProtocolAdapter adapter("ollama", "openai", "llama3", "req_split");
        const std::string first = adapter.feed("{\"message\":{\"content\":\"ab");
        LR_CHECK_MSG(first.empty(), "a partial line must not be emitted: " + first);
        const std::string second = adapter.feed("c\"},\"done\":false}\n");
        LR_CHECK_MSG(second.find("abc") != std::string::npos, second);
    }
    // Ollama's own tool calls arrive whole, and become OpenAI's delta shape.
    {
        StreamProtocolAdapter adapter("ollama", "openai", "llama3", "req_tools");
        const std::string out = adapter.feed(
            "{\"message\":{\"content\":\"\",\"tool_calls\":[{\"function\":{\"name\":\"f\",\"arguments\":{\"a\":1}}}]},\"done\":false}\n");
        LR_CHECK_MSG(out.find("tool_calls") != std::string::npos, out);
        LR_CHECK_MSG(out.find("\"name\":\"f\"") != std::string::npos, out);
    }

    // The CRC32 the framing relies on, pinned to the standard check value.
    LR_CHECK_MSG(crc32Of("123456789") == 0xCBF43926u,
                 std::format("crc32 of the check string is {:08x}", crc32Of("123456789")));

    // Bedrock: binary frames, text and reasoning deltas, then the stop event.
    {
        StreamProtocolAdapter adapter("bedrock", "openai", "m", "req_bedrock");
        const std::string out = adapter.feed(bedrockFrame(
            "contentBlockDelta", R"({"contentBlockIndex":0,"delta":{"text":"Hello"}})"));
        LR_CHECK_MSG(out.find("Hello") != std::string::npos, out);
        LR_CHECK_MSG(out.find("chat.completion.chunk") != std::string::npos, out);
        const std::string reasoning = adapter.feed(bedrockFrame(
            "contentBlockDelta",
            R"({"contentBlockIndex":0,"delta":{"reasoningContent":{"text":"why"}}})"));
        LR_CHECK_MSG(reasoning.find("reasoning_content") != std::string::npos, reasoning);
        const std::string meta = adapter.feed(bedrockFrame(
            "metadata", R"({"usage":{"inputTokens":11,"outputTokens":4}})"));
        // Metadata alone carries no client-visible delta, which is correct: the
        // counts are read by the usage observer, not shown as content.
        LR_CHECK_MSG(meta.find("\"content\"") == std::string::npos, meta);
        const std::string stop =
            adapter.feed(bedrockFrame("messageStop", R"({"stopReason":"end_turn"})"));
        LR_CHECK_MSG(stop.find("[DONE]") != std::string::npos, stop);
        LR_CHECK_MSG(stop.find("\"finish_reason\":\"stop\"") != std::string::npos, stop);
        LR_CHECK_EQ(adapter.finish(), "");
    }
    // Tool use: the start event names the call, and the input arrives as JSON
    // fragments that are forwarded as OpenAI argument fragments.
    {
        StreamProtocolAdapter adapter("bedrock", "openai", "m", "req_bedrock_tools");
        const std::string start = adapter.feed(bedrockFrame(
            "contentBlockStart",
            R"({"contentBlockIndex":0,"start":{"toolUse":{"toolUseId":"tu_9","name":"f"}}})"));
        LR_CHECK_MSG(start.find("tool_calls") != std::string::npos, start);
        LR_CHECK_MSG(start.find("tu_9") != std::string::npos, start);
        LR_CHECK_MSG(start.find("\"name\":\"f\"") != std::string::npos, start);
        const std::string fragment = adapter.feed(bedrockFrame(
            "contentBlockDelta",
            R"({"contentBlockIndex":0,"delta":{"toolUse":{"input":"{\"a\":"}}})"));
        LR_CHECK_MSG(fragment.find("tool_calls") != std::string::npos, fragment);
        LR_CHECK_MSG(fragment.find("arguments") != std::string::npos, fragment);
        const std::string stop =
            adapter.feed(bedrockFrame("messageStop", R"({"stopReason":"tool_use"})"));
        LR_CHECK_MSG(stop.find("\"finish_reason\":\"tool_calls\"") != std::string::npos, stop);
    }
    // Two frames in one chunk are both parsed, and a frame split across two
    // chunks waits for the rest instead of being misread.
    {
        StreamProtocolAdapter adapter("bedrock", "openai", "m", "req_frames");
        const std::string both = bedrockFrame("contentBlockDelta", R"({"delta":{"text":"a"}})") +
                                 bedrockFrame("contentBlockDelta", R"({"delta":{"text":"b"}})");
        const std::string out = adapter.feed(both);
        LR_CHECK_MSG(out.find("\"content\":\"a\"") != std::string::npos, out);
        LR_CHECK_MSG(out.find("\"content\":\"b\"") != std::string::npos, out);

        StreamProtocolAdapter split("bedrock", "openai", "m", "req_half");
        const std::string frame = bedrockFrame("contentBlockDelta", R"({"delta":{"text":"half"}})");
        const std::string head = split.feed(frame.substr(0, 6));
        LR_CHECK_MSG(head.empty(), "half a prelude must not be parsed: " + head);
        const std::string tail = split.feed(frame.substr(6));
        LR_CHECK_MSG(tail.find("half") != std::string::npos, tail);
    }
    // A damaged frame is refused rather than resynchronised on: framing damage
    // must not become plausible-looking model output.
    {
        StreamProtocolAdapter adapter("bedrock", "openai", "m", "req_bad");
        std::string frame = bedrockFrame("contentBlockDelta", R"({"delta":{"text":"tampered"}})");
        frame[frame.size() - 6] ^= 0x01; // inside the payload, before the CRC
        const std::string out = adapter.feed(frame);
        LR_CHECK_MSG(out.find("tampered") == std::string::npos,
                     "a frame with a bad checksum was accepted: " + out);
        // And it stays refused: nothing later is read either.
        const std::string after = adapter.feed(bedrockFrame("contentBlockDelta", R"({"delta":{"text":"later"}})"));
        LR_CHECK_MSG(after.find("later") == std::string::npos, after);
    }
}

} // namespace

int main() {
    testResolvePaths();
    testAnthropicRequestAdaptation();
    testAnthropicResponseAdaptation();
    testGeminiAdaptation();
    testOpenAiResponsesAdaptation();
    testStreamProtocolAdapterAnthropic();
    testStreamProtocolAdapterGemini();
    testStreamProtocolAdapterOpenAiPassthrough();
    testStreamUsageObserver();
    testReasoningMapping();
    testAnthropicIngressAdaptation();
    testGeminiIngressAdaptation();
    testMultiProtocolStreaming();
    testNewOutboundPaths();
    testOllamaAdaptation();
    testBedrockAdaptation();
    testOllamaBedrockStreaming();
    return LR_SUMMARY("test_protocol");
}
