// Private proxy helpers, included after the module imports inside its unnamed
// namespace. Multipart uploads are parsed only as MIME; file contents never
// enter JSON or a text conversion. Keep part order, duplicates and all part
// headers (including filename*) when replaying an upload to another candidate.
// Image/audio SSE events can contain megabytes of base64 in one JSON string.
// The token observer needs the small usage object, not those strings; reduce
// them on its private copy so its bounded event carry never loses a trailing
// usage object. The actual response bypasses this filter entirely.
class MediaUsageFilter {
public:
    std::string feed(std::string_view input) {
        std::string out;
        for (const char ch : input) {
            if (!in_string_) {
                if (ch == '"') {
                    in_string_ = true;
                    string_ = "\"";
                } else {
                    out += ch;
                }
                continue;
            }
            if (!oversized_) {
                string_ += ch;
                if (string_.size() > 128) {
                    string_.clear();
                    oversized_ = true;
                }
            }
            if (ch == '"' && !escaped_) {
                out += oversized_ ? "\"\"" : string_;
                string_.clear();
                in_string_ = false;
                oversized_ = false;
            }
            escaped_ = ch == '\\' && !escaped_;
        }
        return out;
    }

private:
    bool in_string_ = false;
    bool escaped_ = false;
    bool oversized_ = false;
    std::string string_;
};

struct MediaRequest {
    std::vector<h::FormData> parts;
    std::string model;
    bool stream = false;
    std::string boundary;

    std::string contentType() const {
        return "multipart/form-data; boundary=" + boundary;
    }

    std::string metadata() const {
        json out{{"model", model}, {"stream", stream}};
        // No file data in telemetry, even when log_bodies is enabled.
        out["parts"] = json::array();
        for (const auto &part : parts) {
            out["parts"].push_back({{"name", part.name}, {"filename", part.filename},
                                    {"content_type", part.content_type},
                                    {"bytes", part.content.size()}});
        }
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    std::string payload(std::string_view upstream_model) const {
        std::string out;
        bool has_model = false;
        for (const auto &part : parts) {
            out += "--" + boundary + "\r\n";
            for (const auto &[name, value] : part.headers) {
                // A model rename changes its length, so let MIME delimiters
                // frame every part instead of forwarding a stale part length.
                if (toLower(name) != "content-length") {
                    out += name + ": " + value + "\r\n";
                }
            }
            out += "\r\n";
            if (part.name == "model") {
                out += upstream_model;
                has_model = true;
            } else {
                out += part.content;
            }
            out += "\r\n";
        }
        // OpenAI image endpoints default to dall-e-2 if model is omitted. A
        // configured route can still rename that default for the upstream.
        if (!has_model) {
            out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
            out += upstream_model;
            out += "\r\n";
        }
        out += "--" + boundary + "--\r\n";
        return out;
    }
};

std::expected<MediaRequest, std::string> readMediaMultipart(const h::ContentReader &reader) {
    MediaRequest media;
    std::size_t size = 0;
    bool too_large = false;
    if (!reader(
            [&](const h::FormData &part) {
                if (media.parts.size() >= 1024) return false;
                media.parts.push_back(part);
                return true;
            },
            [&](const char *data, std::size_t length) {
                if (media.parts.empty()) return false;
                if (length > kMaxRequestBody - size) {
                    too_large = true;
                    return false;
                }
                size += length;
                media.parts.back().content.append(data, length);
                return true;
            })) {
        return std::unexpected(too_large ? "media upload exceeds 64 MiB" : "malformed multipart body");
    }
    bool found_model = false;
    bool found_stream = false;
    for (const auto &part : media.parts) {
        if (part.name == "model") {
            if (found_model || !part.filename.empty() || part.content.empty()) {
                return std::unexpected("`model` must be a single nonempty text field");
            }
            media.model = part.content;
            found_model = true;
        } else if (part.name == "stream") {
            if (found_stream || !part.filename.empty() ||
                (part.content != "true" && part.content != "false")) {
                return std::unexpected("`stream` must be a single true or false field");
            }
            media.stream = part.content == "true";
            found_stream = true;
        }
    }
    // A random boundary must not accidentally delimit binary file contents.
    do {
        media.boundary = "literouter-" + hexId(24);
    } while (std::ranges::any_of(media.parts, [&](const auto &part) {
        return part.content.find(media.boundary) != std::string::npos;
    }));
    return media;
}
