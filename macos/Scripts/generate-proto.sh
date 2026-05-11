#!/bin/bash
# Generate Swift protobuf sources from .proto files.
# Requires: protoc + swift-protobuf plugin (protoc-gen-swift)
#
# Install: brew install swift-protobuf
#
# Usage: ./macos/Scripts/generate-proto.sh   (from repo root)
#        ./Scripts/generate-proto.sh         (from macos/)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MACOS_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$MACOS_DIR")"
PROTO_DIR="$REPO_ROOT/Proto"
OUT_DIR="$MACOS_DIR/Sources/ClipboardHelper/Generated"

mkdir -p "$OUT_DIR"

echo "Generating Swift protobuf sources..."

protoc \
    --proto_path="$PROTO_DIR" \
    --swift_out="$OUT_DIR" \
    --swift_opt=Visibility=Public \
    "$PROTO_DIR/clipboard.proto" \
    "$PROTO_DIR/clipboard_helper.proto"

echo "Generated files in $OUT_DIR:"
ls -la "$OUT_DIR"/*.pb.swift
