#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

flags=(-std=c++20 -pthread -O3 -Wall -Wextra -Iinclude)
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "Building for macOS ($(uname -m))..."
elif [[ "$(uname -s)" == "Linux" ]]; then
  echo "Building for Linux ($(uname -m))..."
fi

arch="$(uname -m)"
if [[ "$arch" == "x86_64" || "$arch" == "amd64" ]]; then
  flags+=(-mavx2 -mfma)
fi

clang++ "${flags[@]}" src/main.cpp -o munxc
echo "Built ./munxc"
