#!/usr/bin/env bash
set -euo pipefail

git pull --ff-only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/hello
