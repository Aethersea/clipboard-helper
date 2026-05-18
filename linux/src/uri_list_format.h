#pragma once
//
// uri_list_format — pure helpers for formatting file paths into
// clipboard MIME payloads, shared by the Wayland (wlr / ext) and X11
// backends.
//
// Both backends used to carry duplicate copies of the percent-encoder
// and the text/uri-list / x-special/gnome-copied-files formatters.
// They're pure byte-to-byte functions with no Qt or libwayland
// dependencies, so extracting them lets us:
//   - guarantee identical behaviour across all three backends.
//   - unit-test the encoding rules in isolation.
//   - keep the wire-format spec (RFC 2483, GNOME Nautilus convention)
//     documented in exactly one place.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace leviathan::clipboard_helper::uri_list {

// Mark the bytes that are safe to include unescaped inside a file://
// URI's path segment, per RFC 3986 unreserved set plus the path
// separator '/'.
//   unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
//   "/" is technically reserved but is the path separator we want to
//   preserve.
// Everything else (including UTF-8 continuation bytes, '#', '?', ' '
// etc.) gets percent-encoded.
bool IsUriPathSafe(unsigned char c);

// Percent-encode `path` byte-by-byte (UTF-8 sequences are encoded one
// byte at a time, which is what RFC 3986 specifies). Returns a string
// of printable ASCII suitable for inclusion in a URI.
std::string PercentEncodePath(std::string_view path);

// Parse the parent's PROVIDE_DATA payload, which is a list of LOCAL
// absolute paths separated by '\n' (the format the macOS helper's
// send_provide_data_file_paths emits on the shen Rust side). Empty
// lines are skipped; trailing '\r' on input is tolerated.
std::vector<std::string> SplitPaths(std::string_view path_bytes);

// Build a `file://` URI from an absolute local path. Just
// "file://" + PercentEncodePath(path).
std::string MakeFileUri(std::string_view path);

// Format paths into a text/uri-list payload (RFC 2483):
// each file:// URI terminated by CRLF, including the last one.
std::string FormatTextUriList(const std::vector<std::string>& paths);

// Format paths into the x-special/gnome-copied-files payload that
// Nautilus / GTK file managers consume:
//   "copy" + LF + URI + LF + URI + ... (no trailing terminator)
// Shen only ever produces copies, so the operation header is
// hard-coded to "copy".
std::string FormatGnomeCopiedFiles(const std::vector<std::string>& paths);

// Convenience wrapper combining SplitPaths + Format{TextUriList,
// GnomeCopiedFiles} based on `mime`. Unknown MIMEs fall back to
// text/uri-list since the caller (a wayland send handler or X11
// retrieveData) should only ever pass MIMEs we advertised.
std::vector<std::uint8_t> FormatForMime(const std::vector<std::uint8_t>& path_bytes,
                                        std::string_view mime);

}  // namespace leviathan::clipboard_helper::uri_list
