set shell := ["bash", "-euo", "pipefail", "-c"]

build_dir := "build"

default:
    @just --list

# Configure (assumes you're already in nix develop)
configure:
    cmake -S . -B {{build_dir}} -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCMAKE_CXX_COMPILER="$CXX"

# Build
build:
    cmake --build {{build_dir}}

# Clean build dir
clean:
    rm -rf {{build_dir}}

# Reconfigure from scratch
reconfigure: clean configure

# Symlink compile_commands.json for clangd
compdb:
    ln -sf {{build_dir}}/compile_commands.json compile_commands.json

# Normal workflow
all: configure build compdb
