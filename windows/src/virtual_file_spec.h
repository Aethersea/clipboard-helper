#pragma once
//
// VirtualFileSpec — per-file metadata used by both the codec (decoding
// FileMetadata sub-messages on the announce path) and the IDataObject
// server (publishing CFSTR_FILEDESCRIPTORW).
//
// Carved out of virtual_file_provider.h so the codec TU can include this
// without pulling in <ole2.h>.

#include <cstdint>
#include <string>

namespace leviathan::clipboard_helper {

// Per-file metadata used to populate the CFSTR_FILEDESCRIPTORW group.
//
// `name`         — file path the consumer sees. May include backslash-
//                  separated subdirectories when copying a folder tree;
//                  Windows' virtual-file shell consumers split on '\\'
//                  to build the destination layout. Truncated to MAX_PATH-1
//                  chars when copied into FILEDESCRIPTORW.cFileName.
// `size`         — fills FILEDESCRIPTORW.nFileSize{Low,High}. 0 for dirs.
// `file_id`      — opaque token forwarded to ChunkProvider::FetchChunk.
//                  Matches FileMetadata.file_id from the announcement
//                  protobuf, so the parent can resolve it back to the
//                  upstream WebRTC source.
// `is_directory` — selects FILE_ATTRIBUTE_DIRECTORY vs FILE_ATTRIBUTE_NORMAL
//                  in dwFileAttributes. Directories should still appear
//                  with their entire descendant set as additional specs
//                  (FILEGROUPDESCRIPTOR enumerates files; the shell
//                  reconstructs the tree from sibling entries' relative
//                  paths).
struct VirtualFileSpec {
    std::wstring  name;
    std::uint64_t size{0};
    std::string   file_id;
    bool          is_directory{false};
};

}  // namespace leviathan::clipboard_helper
