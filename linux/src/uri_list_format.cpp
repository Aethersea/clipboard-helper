#include "uri_list_format.h"

#include <cstdio>
#include <cstring>
#include <utility>

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

namespace {

char AsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
    }
    return true;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Whitespace plus NUL: some X11 owners NUL-terminate their selection
// bytes, and a stray space around a URI is not part of it (a literal
// space inside a URI must be %20-encoded).
bool IsLineTrim(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsLineTrim(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsLineTrim(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

std::optional<std::string> DecodeLocalFileUri(std::string_view uri) {
    constexpr std::string_view kScheme = "file:";
    if (uri.size() < kScheme.size()
        || !EqualsIgnoreCase(uri.substr(0, kScheme.size()), kScheme)) {
        return std::nullopt;
    }
    std::string_view rest = uri.substr(kScheme.size());

    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        // Authority present: only the local host is acceptable. A
        // file:// URI naming another machine is not something we can
        // stat or serve, so the whole list stays TEXT.
        rest.remove_prefix(2);
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) return std::nullopt;
        const std::string_view host = rest.substr(0, slash);
        if (!host.empty() && !EqualsIgnoreCase(host, "localhost")) {
            return std::nullopt;
        }
        rest.remove_prefix(slash);
    }
    // Absolute paths only ("file:relative" is not a usable location).
    if (rest.empty() || rest[0] != '/') return std::nullopt;

    // A raw '?' or '#' starts a query / fragment; file managers
    // percent-encode those characters in names, so seeing one means the
    // URI is not a plain local path. Refuse rather than guess.
    if (rest.find_first_of("?#") != std::string_view::npos) return std::nullopt;

    std::string path;
    path.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const char c = rest[i];
        if (c != '%') {
            path += c;
            continue;
        }
        if (i + 2 >= rest.size()) return std::nullopt;
        const int hi = HexValue(rest[i + 1]);
        const int lo = HexValue(rest[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        const char decoded = static_cast<char>((hi << 4) | lo);
        // An embedded NUL can never be part of a POSIX path.
        if (decoded == '\0') return std::nullopt;
        path += decoded;
        i += 2;
    }

    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

std::optional<std::vector<std::string>> ParseLocalFileUriList(std::string_view uri_list) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= uri_list.size()) {
        std::size_t end = uri_list.find('\n', start);
        if (end == std::string_view::npos) end = uri_list.size();
        const std::string_view line = Trim(uri_list.substr(start, end - start));
        // RFC 2483: lines starting with '#' are comments.
        if (!line.empty() && line.front() != '#') {
            auto path = DecodeLocalFileUri(line);
            if (!path) return std::nullopt;
            paths.push_back(std::move(*path));
        }
        if (end >= uri_list.size()) break;
        start = end + 1;
    }
    if (paths.empty()) return std::nullopt;
    return paths;
}

}  // namespace leviathan::clipboard_helper::uri_list
