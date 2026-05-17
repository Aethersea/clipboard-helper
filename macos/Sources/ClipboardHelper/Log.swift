import Foundation

/// Simple logging utility that writes to stderr.
///
/// The pure formatter (`formatLine`) and the I/O sink (`writeSink`) are
/// split so tests can drive the formatter without redirecting stderr,
/// and so tests can capture the bytes Log would have written to stderr
/// by substituting their own sink. Production callers still see the
/// same `Log.info / Log.warning / Log.error / Log.debug` surface.
enum Log {
    static var verbose = false

    // Bytes-out sink. Tests reassign this in setUp + restore in tearDown
    // to capture what would have been written; production keeps the
    // stderr default. The closure form mirrors the dependency-injection
    // pattern used by FileTransferCoordinator.sendMessage.
    static var writeSink: (Data) -> Void = { Log.defaultWriteSink($0) }

    private static func defaultWriteSink(_ data: Data) {
        FileHandle.standardError.write(data)
    }

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

    /// Build the textual log line for `message` at `level`, stamped with
    /// `date`. Pure: no I/O, no clock read, no global state. Layout:
    /// `[<iso8601>] [<level>] <message>\n`.
    static func formatLine(level: String, message: String, date: Date) -> String {
        let timestamp = ISO8601DateFormatter().string(from: date)
        return "[\(timestamp)] [\(level)] \(message)\n"
    }

    private static func log(_ level: String, _ message: String) {
        let line = formatLine(level: level, message: message, date: Date())
        writeSink(Data(line.utf8))
    }
}
