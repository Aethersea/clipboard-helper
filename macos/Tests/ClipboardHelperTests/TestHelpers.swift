import Foundation
@testable import ClipboardHelper

enum TestHelpers {
    static let chunkSize: Int = 256 * 1024
    static let pipelineDepth: Int = 4

    static func makeAnnouncement(
        transferID: String,
        contentHash: String,
        files: [Leviathan_FileMetadata]
    ) -> Leviathan_ClipboardAnnouncement {
        var ann = Leviathan_ClipboardAnnouncement()
        ann.contentType = .files
        ann.transferID = transferID
        ann.contentHash = contentHash
        ann.files = files
        return ann
    }

    static func makeFileMetadata(
        id: String,
        name: String,
        size: UInt64
    ) -> Leviathan_FileMetadata {
        var m = Leviathan_FileMetadata()
        m.fileID = id
        m.filename = name
        m.fileSize = size
        return m
    }

    static func makeChunk(
        transferID: String,
        fileID: String,
        offset: UInt64,
        data: Data,
        isLast: Bool = false,
        error: String = ""
    ) -> Leviathan_HelperFileChunkData {
        var c = Leviathan_HelperFileChunkData()
        c.transferID = transferID
        c.fileID = fileID
        c.offset = offset
        c.data = data
        c.isLast = isLast
        c.error = error
        return c
    }

    static func payload(byteCount: Int) -> Data {
        var data = Data(count: byteCount)
        data.withUnsafeMutableBytes { raw in
            guard let base = raw.baseAddress else { return }
            let ptr = base.assumingMemoryBound(to: UInt8.self)
            for i in 0..<byteCount {
                ptr[i] = UInt8(i & 0xFF)
            }
        }
        return data
    }

    static func slice(of payload: Data, offset: UInt64, maxLength: Int) -> Data {
        let start = Int(offset)
        let end = min(start + maxLength, payload.count)
        guard start < end else { return Data() }
        return payload.subdata(in: start..<end)
    }
}
