// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "ClipboardHelper",
    platforms: [
        .macOS(.v13)
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-protobuf.git", from: "1.28.0"),
        .package(url: "https://github.com/apple/swift-argument-parser.git", "1.3.0"..<"1.5.0"),
    ],
    targets: [
        .executableTarget(
            name: "ClipboardHelper",
            dependencies: [
                .product(name: "SwiftProtobuf", package: "swift-protobuf"),
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ],
            path: "Sources/ClipboardHelper"
        ),
        .testTarget(
            name: "ClipboardHelperTests",
            dependencies: ["ClipboardHelper"],
            path: "Tests/ClipboardHelperTests"
        ),
    ]
)
