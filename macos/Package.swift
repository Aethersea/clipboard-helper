// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "ClipboardHelper",
    platforms: [
        .macOS(.v13)
    ],
    dependencies: [
        // Pinned exact, kept in lockstep with the protoc-gen-swift version the
        // CI macOS job builds from source (see .github/workflows/build.yml) so
        // generated code always matches the linked runtime. 1.34.1 is the last
        // line whose generated output compiles under the macos-14 runner's
        // Swift 5.10: 1.38.0+ emits type-/extension-level `nonisolated`
        // (SE-0449), which 5.10 only supports at member level. It also ships a
        // Package@swift-5.10.swift manifest, so the runtime builds on 5.10.
        .package(url: "https://github.com/apple/swift-protobuf.git", exact: "1.34.1"),
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
