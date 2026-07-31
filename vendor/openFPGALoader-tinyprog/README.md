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
  (regenerated, not vendored). Native rebuild (on the machine you'll run it
  on) uses the usual CMake flow:
  ```bash
  cd source
  cmake -B build
  cmake --build build
  ```
  Build deps (macOS): `brew install cmake pkg-config libusb libftdi hidapi`.
- `prebuilt/darwin-arm64/` — built natively on an Apple Silicon Mac.
  **macOS arm64 only** — it's dynamically linked against Homebrew dylibs at
  absolute paths (`/opt/homebrew/opt/...`), confirmed via `otool -L`. Won't
  run on Intel Mac, Linux, or Windows, and won't even run on another arm64
  Mac unless the exact same Homebrew libs are installed there.
- `prebuilt/win32-x64/`, `prebuilt/linux-x64/`, `prebuilt/linux-arm64/` —
  cross-compiled from macOS using [zig](https://ziglang.org/) as the C/C++
  toolchain (see below). Windows binary is statically linked (no DLLs to
  ship). Linux binaries dynamically link only glibc/libdl/libpthread —
  nothing distro-specific — everything else (libusb, libftdi, zlib) is
  statically embedded.

Every `prebuilt/<platform>-<arch>/` dir also carries the runtime sidecar
assets (`spiOverJtag_*.gz`/`.bit.gz`) — identical across platforms, just
copied over.

## Cross-building with zig

Cross-compiling this fork isn't just "point a different compiler at it" —
getting all three targets (Windows, Linux x64, Linux arm64) working
surfaced several real bugs and toolchain quirks, fixed along the way:

- **`src/tinyprog.cpp`**: `disconnect()` referenced the `_usb` member
  unconditionally, but that member only exists `#ifdef __APPLE__` (it backs
  the macOS-only raw-bulk-USB workaround for a CDC-ACM driver quirk — see
  below). Broke compilation on every non-Apple target. Fixed by wrapping the
  block in the same `#ifdef __APPLE__`.
- **`src/windows_stuff.cpp`**: used `std::stringstream` without including
  `<sstream>`. Fixed.
- **`CMakeLists.txt`**: `-DCPPHTTPLIB_OPENSSL_SUPPORT` was added to the
  Windows/Linux/macOS build flags unconditionally, but OpenSSL is only
  *linked* `if(OPENSSL_FOUND)` — a real inconsistency, not a cross-compile
  workaround. Without OpenSSL available (the normal case when
  cross-compiling, since we don't cross-build OpenSSL), the macro was set
  but no headers/libs backed it, breaking the build. Fixed by gating the
  macro the same way as the linking. **Effect: cross-compiled binaries lack
  the TLS-based firmware-update-check HTTP client feature.** Core flashing
  and the blank-bootmeta fallback are unaffected — that logic
  (`tinyprog.cpp` around `BX_DEFAULT_*`) is pure JSON parsing with no
  platform or TLS dependency.
- **zig's default archiver (`zig ar`) needs `--format=gnu` explicitly.**
  Without it, for a Windows target it produces a COFF-style archive whose
  symbol index `lld-link`'s mingw driver can't use — silently causing
  "undefined symbol" for functions that genuinely are in the archive.
- **`zig cc` enables a full UBSan-with-runtime-handlers set by default**
  (its `-O0`/Debug-equivalent mode). Those runtime handler symbols
  (`__ubsan_handle_*`) aren't auto-linked when the wrapper is only used to
  build intermediate static libs, causing undefined-symbol errors at final
  link. Fixed by always passing `-fno-sanitize=undefined`.
- **glibc requires dynamic linking** — zig's bundled glibc doesn't support
  `-static` final executables. `BUILD_STATIC` is off for both Linux
  toolchain files; libusb/libftdi/zlib still link in statically (they're
  only available as `.a` in the cross-deps prefix), only glibc itself stays
  dynamic.
- **libftdi1's own build always produces both a static `.a` and a shared
  `.so`.** If both are present in the same lib dir, the linker prefers the
  `.so` — meaning without removing it, the resulting binary would need
  `libftdi1.so.2` present on the target machine (which it won't have). The
  shared lib is deliberately not installed into the Linux cross-deps
  prefixes for this reason.

### Toolchain files

- `source/cmake/Toolchain-x86_64-w64-mingw32.cmake` — pre-existing in this
  fork, works unmodified with zig; it does `find_program(NAMES
  x86_64-w64-mingw32-gcc ...)`, so `source/cmake/zig-cc/` ships wrapper
  scripts under those exact names that shell out to `zig cc -target
  x86_64-windows-gnu`.
- `source/cmake/Toolchain-x86_64-linux-gnu-zig.cmake` /
  `Toolchain-aarch64-linux-gnu-zig.cmake` — new, written for this
  cross-build effort. Same wrapper-script pattern, targeting
  `{x86_64,aarch64}-linux-gnu.2.28` (glibc 2.28 floor for wide distro
  compat).
- `source/cmake/zig-cc/zig-ar` / `zig-ranlib` — wrap `zig ar
  --format=gnu` / `zig ranlib`, used as `CMAKE_AR`/`CMAKE_RANLIB` for every
  cross target (macOS's own `ar` can't read the `@response-file` argument
  lists CMake emits for some of these builds).

### Rebuilding

Requires `zig`, `p7zip` (for libusb's Windows release archive), and
`llvm` (for `llvm-windres`/`llvm-strip`) — all via `brew install zig p7zip
llvm`.

For each target, `libusb`, `libftdi1`, and (Linux/Windows only) `zlib` need
cross-building first into a "cross-deps" install prefix — put
`include/`, `lib/*.a`, and `lib/pkgconfig/*.pc` there — then:

```bash
# Windows (uses the pre-existing toolchain file's own libusb/libftdi/zlib
# auto-fetch — set CROSS_COMPILE_DEPS=OFF and point at your own prefix instead,
# as done here, if you've already built your own copies)
export PATH="source/cmake/zig-cc:$PATH"
cmake -B build-win64 -S source \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-x86_64-w64-mingw32.cmake \
  -DWINDOWS_CROSSCOMPILE=ON -DCROSS_COMPILE_DEPS=OFF \
  -DCROSS_DEPS_DIR=<cross-deps-prefix> -DCMAKE_PREFIX_PATH=<cross-deps-prefix> \
  -DCMAKE_AR=source/cmake/zig-cc/zig-ar -DCMAKE_RANLIB=source/cmake/zig-cc/zig-ranlib \
  -DENABLE_TINYPROG=ON
cmake --build build-win64 --parallel

# Linux (x64 shown; swap Toolchain-aarch64-linux-gnu-zig.cmake for arm64)
export PKG_CONFIG_LIBDIR=<cross-deps-prefix>/lib/pkgconfig
export PKG_CONFIG_PATH=
cmake -B build-linux-x64 -S source \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-x86_64-linux-gnu-zig.cmake \
  -DCROSS_DEPS_INSTALL_DIR=<cross-deps-prefix> -DCMAKE_PREFIX_PATH=<cross-deps-prefix> \
  -DENABLE_TINYPROG=ON
cmake --build build-linux-x64 --parallel
```

## Known limitations

- Cross-compiled Windows/Linux binaries have not been exercised on real
  Windows/Linux hardware yet — only verified via structural checks
  (file format, symbol resolution, dynamic-dependency list) on the build
  machine. The Windows serial-port tinyprog path (`src/uart_ll.cpp`,
  `src/windows_stuff.cpp`) is real (non-stub) code but likewise untested on
  actual Windows.
- No TLS/HTTPS support in cross-compiled binaries (see above) — the
  tinyprog firmware-update-check feature is unavailable there. Everything
  else, including the blank-bootmeta default-address-map fallback that's
  the whole point of this fork, is unaffected.
- CMSIS-DAP (`ENABLE_CMSISDAP`, needs `hidapi`) is disabled in all
  cross-compiled binaries — hidapi wasn't cross-built.
