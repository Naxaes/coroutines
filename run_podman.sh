#!/bin/bash
set -euo pipefail

IMAGE_NAME=coroutine-test

echo "Building container image..."
podman build -t "$IMAGE_NAME" .

echo "Running container..."
podman run --rm --name coroutine-container "$IMAGE_NAME"
