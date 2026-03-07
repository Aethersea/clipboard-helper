import Foundation
import SwiftProtobuf

/// Manages a Unix domain socket connection for IPC with the parent process.
/// Uses length-prefixed (4-byte little-endian) protobuf framing.
final class SocketServer {
    private let socketPath: String
    private var listenSocket: Int32 = -1
    private var clientSocket: Int32 = -1
    private var readSource: DispatchSourceRead?
    private let queue = DispatchQueue(label: "com.leviathan.clipboard-helper.socket", qos: .userInitiated)
    private let writeQueue = DispatchQueue(label: "com.leviathan.clipboard-helper.socket.write", qos: .userInitiated)
    private var readBuffer = Data()

    var onMessage: ((Leviathan_HelperMessage) -> Void)?
    var onClientConnected: (() -> Void)?
    var onClientDisconnected: (() -> Void)?

    init(socketPath: String) {
        self.socketPath = socketPath
    }

    deinit {
        stop()
    }

    // MARK: - Lifecycle

    func start() throws {
        // Remove stale socket file
        unlink(socketPath)

        listenSocket = socket(AF_UNIX, SOCK_STREAM, 0)
        guard listenSocket >= 0 else {
            throw SocketError.createFailed(errno: errno)
        }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = socketPath.utf8CString
        guard pathBytes.count <= MemoryLayout.size(ofValue: addr.sun_path) else {
            throw SocketError.pathTooLong
        }
        withUnsafeMutablePointer(to: &addr.sun_path) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: pathBytes.count) { dest in
                for (i, byte) in pathBytes.enumerated() {
                    dest[i] = byte
                }
            }
        }

        let bindResult = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                bind(listenSocket, sockPtr, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bindResult == 0 else {
            throw SocketError.bindFailed(errno: errno)
        }

        // Restrict socket permissions to owner only
        chmod(socketPath, 0o600)

        guard listen(listenSocket, 1) == 0 else {
            throw SocketError.listenFailed(errno: errno)
        }

        Log.info("Socket listening on \(socketPath)")
        acceptConnection()
    }

    func stop() {
        readSource?.cancel()
        readSource = nil

        if clientSocket >= 0 {
            close(clientSocket)
            clientSocket = -1
        }
        if listenSocket >= 0 {
            close(listenSocket)
            listenSocket = -1
        }

        unlink(socketPath)
    }

    // MARK: - Send

    func send(_ message: Leviathan_HelperMessage) {
        writeQueue.async { [weak self] in
            guard let self = self, self.clientSocket >= 0 else { return }
            do {
                let data = try message.serializedData()
                var length = UInt32(data.count).littleEndian
                let header = Data(bytes: &length, count: 4)

                self.writeAll(header)
                self.writeAll(data)
            } catch {
                Log.error("Failed to serialize message: \(error)")
            }
        }
    }

    // MARK: - Private

    private func acceptConnection() {
        queue.async { [weak self] in
            guard let self = self, self.listenSocket >= 0 else { return }

            let fd = accept(self.listenSocket, nil, nil)
            guard fd >= 0 else {
                if errno != EINVAL && errno != EBADF {
                    Log.error("Accept failed: \(errno)")
                }
                return
            }

            Log.info("Client connected")
            self.clientSocket = fd
            self.readBuffer = Data()
            self.startReading()

            DispatchQueue.main.async {
                self.onClientConnected?()
            }
        }
    }

    private func startReading() {
        let source = DispatchSource.makeReadSource(fileDescriptor: clientSocket, queue: queue)

        source.setEventHandler { [weak self] in
            self?.handleReadEvent()
        }

        source.setCancelHandler { [weak self] in
            guard let self = self else { return }
            if self.clientSocket >= 0 {
                close(self.clientSocket)
                self.clientSocket = -1
            }
            Log.info("Client disconnected")
            DispatchQueue.main.async {
                self.onClientDisconnected?()
            }
            // Accept next connection
            self.acceptConnection()
        }

        self.readSource = source
        source.resume()
    }

    private func handleReadEvent() {
        var buf = [UInt8](repeating: 0, count: 65536)
        let n = read(clientSocket, &buf, buf.count)

        if n <= 0 {
            readSource?.cancel()
            return
        }

        readBuffer.append(contentsOf: buf[0..<n])
        processFrames()
    }

    private func processFrames() {
        while readBuffer.count >= 4 {
            let length = readBuffer.withUnsafeBytes { ptr -> UInt32 in
                ptr.load(as: UInt32.self).littleEndian
            }

            // Sanity check: reject frames larger than 64MB
            guard length <= 64 * 1024 * 1024 else {
                Log.error("Frame too large: \(length) bytes, disconnecting")
                readSource?.cancel()
                return
            }

            let totalNeeded = 4 + Int(length)
            guard readBuffer.count >= totalNeeded else {
                return // Need more data
            }

            let messageData = readBuffer.subdata(in: 4..<totalNeeded)
            readBuffer = readBuffer.subdata(in: totalNeeded..<readBuffer.count)

            do {
                let message = try Leviathan_HelperMessage(serializedBytes: messageData)
                DispatchQueue.main.async { [weak self] in
                    self?.onMessage?(message)
                }
            } catch {
                Log.error("Failed to parse message: \(error)")
            }
        }
    }

    private func writeAll(_ data: Data) {
        data.withUnsafeBytes { ptr in
            guard let baseAddress = ptr.baseAddress else { return }
            var written = 0
            while written < data.count {
                let n = write(clientSocket, baseAddress + written, data.count - written)
                if n <= 0 {
                    Log.error("Write failed: \(errno)")
                    return
                }
                written += n
            }
        }
    }
}

// MARK: - Errors

enum SocketError: Error, CustomStringConvertible {
    case createFailed(errno: Int32)
    case bindFailed(errno: Int32)
    case listenFailed(errno: Int32)
    case pathTooLong

    var description: String {
        switch self {
        case .createFailed(let e): return "Failed to create socket: \(String(cString: strerror(e)))"
        case .bindFailed(let e): return "Failed to bind socket: \(String(cString: strerror(e)))"
        case .listenFailed(let e): return "Failed to listen on socket: \(String(cString: strerror(e)))"
        case .pathTooLong: return "Socket path exceeds maximum length"
        }
    }
}
