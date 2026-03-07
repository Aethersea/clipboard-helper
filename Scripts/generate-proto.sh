#!/bin/bash
# Generate Swift protobuf sources from .proto files.
# Requires: protoc + swift-protobuf plugin (protoc-gen-swift)
#
# Install: brew install swift-protobuf
#
# Usage: ./Scripts/generate-proto.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PROTO_DIR="$PROJECT_DIR/Proto"
OUT_DIR="$PROJECT_DIR/Sources/ClipboardHelper/Generated"

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
