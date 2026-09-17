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

    // ── The direction not taken, stated rather than implied ─────────────────
    {
        // An OpenAI relay's reasoning deltas are NOT turned into an Anthropic
        // thinking block: that needs a second content block with its own index
        // and start/stop frames, and a block sequence that is wrong is worse to
        // a strict client than a missing one. What matters is that the stream
        // stays valid Anthropic rather than malformed.
        StreamProtocolAdapter adapter("openai", "anthropic", "gpt-4o", "req_reverse");
        const std::string out = adapter.feed(
            "data: {\"id\":\"c\",\"choices\":[{\"delta\":{\"reasoning_content\":\"hidden\"}}]}\n\n");
        LR_CHECK(out.find("thinking") == std::string::npos);
        LR_CHECK(out.find("message_start") != std::string::npos);
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
    return LR_SUMMARY("test_protocol");
}
