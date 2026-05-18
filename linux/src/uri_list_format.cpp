#include "uri_list_format.h"

#include <cstdio>
#include <cstring>

namespace leviathan::clipboard_helper::uri_list {

bool IsUriPathSafe(unsigned char c) {
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~'
        || c == '/';
}

std::string PercentEncodePath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if (IsUriPathSafe(c)) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<std::string> SplitPaths(std::string_view path_bytes) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= path_bytes.size()) {
        std::size_t end = path_bytes.find('\n', start);
        if (end == std::string_view::npos) end = path_bytes.size();
        std::string_view line = path_bytes.substr(start, end - start);
        // Tolerate '\r\n' on input by stripping a trailing CR.
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) {
            out.emplace_back(line);
        }
        if (end >= path_bytes.size()) break;
        start = end + 1;
    }
    return out;
}

std::string MakeFileUri(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 7);
    out += "file://";
    out += PercentEncodePath(path);
    return out;
}

std::string FormatTextUriList(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) {
        out += MakeFileUri(p);
        out += "\r\n";
    }
    return out;
}

std::string FormatGnomeCopiedFiles(const std::vector<std::string>& paths) {
    std::string out;
    // Nautilus format: "copy" + LF + LF-joined URIs. NO trailing LF
    // (Nautilus's parser stops at the last URI; a trailing LF would
    // produce a phantom empty URI entry).
    out += "copy";
    for (const auto& p : paths) {
        out += '\n';
        out += MakeFileUri(p);
    }
    return out;
}

std::vector<std::uint8_t> FormatForMime(const std::vector<std::uint8_t>& path_bytes,
                                        std::string_view mime) {
    const auto paths = SplitPaths(std::string_view(
        reinterpret_cast<const char*>(path_bytes.data()), path_bytes.size()));

    std::string out;
    if (mime == std::string_view("x-special/gnome-copied-files")) {
        out = FormatGnomeCopiedFiles(paths);
    } else {
        // text/uri-list and any unexpected mime fall through to the
        // RFC 2483 representation. Backends should only ever pass
        // MIMEs they advertised, so the fallback is just safety.
        out = FormatTextUriList(paths);
    }
    return std::vector<std::uint8_t>(out.begin(), out.end());
}

}  // namespace leviathan::clipboard_helper::uri_list
