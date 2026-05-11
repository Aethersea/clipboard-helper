import Foundation

/// Simple logging utility that writes to stderr.
enum Log {
    static var verbose = false

    static func info(_ message: String) {
        log("INFO", message)
    }

    static func warning(_ message: String) {
        log("WARN", message)
    }

    static func error(_ message: String) {
        log("ERROR", message)
    }

    static func debug(_ message: String) {
        guard verbose else { return }
        log("DEBUG", message)
    }

    private static func log(_ level: String, _ message: String) {
        let timestamp = ISO8601DateFormatter().string(from: Date())
        FileHandle.standardError.write(Data("[\(timestamp)] [\(level)] \(message)\n".utf8))
    }
}
