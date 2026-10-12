#!/usr/bin/env bash
# Build the firmware in the official ESP-IDF Docker image.
# Usage: ./build.sh            -> idf.py build
#        ./build.sh clean      -> any other idf.py command
# Output goes to build-docker/ (kept separate from a native build/ directory).
set -euo pipefail

IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.5}"   # keep in sync with the project's IDF version
BUILD_DIR="${BUILD_DIR:-build-docker}"
cd "$(dirname "$0")"

[ -f main/secrets.h ] || { echo "Missing main/secrets.h (copy main/secrets.example.h and fill it out)"; exit 1; }

args=(
  run --rm
  -v "$PWD:/project" -w /project
  -u "$(id -u):$(id -g)"
  -e HOME=/tmp
  "$IDF_IMAGE"
  bash -c 'git config --global --add safe.directory /project && idf.py -B "$0" "${@:-build}"'
  "$BUILD_DIR" "$@"
)

docker "${args[@]}"