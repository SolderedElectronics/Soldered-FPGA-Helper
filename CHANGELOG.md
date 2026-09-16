# Changelog

All notable changes to this extension are documented here.

## [Unreleased]

## [1.0.1]

### Fixed
- Replaced the marketplace icon (was the plain Soldered logo) with the FPGA chip icon.
- Fixed the release workflow racing all platform targets in parallel on a brand-new extension's first publish, which left some platforms (including Mac Silicon) without a working package.

## [1.0.0]

### Fixed
- Removed the "Graphical Build" button. Upstream apio dropped `build --gui` with no direct replacement, so any fresh install broke this feature.
- The bundled apio toolchain now checks for an update once per VS Code session instead of only on first install, so users no longer get stuck on whatever version happened to be latest when their environment was first set up.

## [0.1.0]

### Added
- Create, open, and scaffold FPGA projects for Soldered boards, starting with TinyFPGA BX.
- Build, upload, simulate, and test commands from the panel.
- Automatic FPGA board detection over USB/serial.
- Bundled, cross-compiled openFPGALoader for Windows, Linux, and macOS.
- Soldered branding and marketplace metadata.
