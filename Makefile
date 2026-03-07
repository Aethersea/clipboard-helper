.PHONY: build build-release build-universal clean generate-proto

# Debug build
build:
	swift build

# Release build (current arch)
build-release:
	swift build -c release

# Universal binary (arm64 + x86_64) for distribution
build-universal:
	swift build -c release --arch arm64 --arch x86_64

# Generate Swift protobuf sources
generate-proto:
	bash Scripts/generate-proto.sh

# Clean build artifacts
clean:
	swift package clean
	rm -rf .build

# Install the helper binary to /usr/local/bin (for development)
install: build-release
	cp .build/release/ClipboardHelper /usr/local/bin/clipboard-helper

# Run with a test socket
run:
	swift run ClipboardHelper --socket /tmp/clipboard-helper.sock --verbose
