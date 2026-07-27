# openFPGALoader (patched, tinyprog fallback) — temporary vendoring

This is a stopgap. The real home for this should be a proper fork on GitHub
(or prebuilt release binaries hosted there) — it's vendored directly into
this repo for now just to get flashing unblocked while that gets sorted out.

## Why this exists

TinyFPGA BX boards with blank SPI flash security registers (no factory
bootmeta) crash both stock `openFPGALoader` and the official `tinyprog`
Python tool. This patched build (see `source/src/tinyprog.cpp`) falls back
to a default flash address map instead of failing. See the project's
`docs/tinyfpga-bx.md` (once written) for the full hardware background.

## Layout

- `source/` — full patched source tree, minus the `build/` directory
  (regenerated, not vendored). Rebuild with the usual CMake flow:
  ```bash
  cd source
  cmake -B build
  cmake --build build
  ```
  Build deps (macOS): `brew install cmake pkg-config libusb libftdi hidapi`.
- `prebuilt/darwin-arm64/` — the compiled binary plus its required runtime
  sidecar assets (`spiOverJtag_*.gz`/`.bit.gz`), built on an Apple Silicon
  Mac. **macOS arm64 only** — it's dynamically linked against Homebrew
  dylibs at absolute paths (`/opt/homebrew/opt/...`), confirmed via
  `otool -L`. It will not run on Intel Mac, Linux, or Windows, and won't
  even run on another arm64 Mac unless the exact same Homebrew libs are
  installed there.

## Known limitation

No prebuilt binaries exist yet for other platforms. Anyone not on an
Apple Silicon Mac with this exact Homebrew setup needs to rebuild from
`source/` themselves for now.
