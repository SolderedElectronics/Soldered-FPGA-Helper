# Changelog

All notable changes to this extension are documented here.

## [Unreleased]

## [1.0.6]

### Fixed
- Scaled up the marketplace icon so it fills the canvas properly instead of looking small/off-center in the Extensions list.

## [1.0.5]

### Fixed
- Updated the activity bar icon to the logic gate design with a Soldered S cutout, matching the marketplace icon.
- Fixed the local F5 debug build task, which still referenced the renamed `vscode:prepublish` script.

## [1.0.4]

### Fixed
- Marketplace icon now includes the Soldered S logo.
- Removed a stray separator between the Soldered and GitHub links in the panel footer.

## [1.0.3]

### Fixed
- The extension no longer hangs after a successful upload to the board.
- Replaced the marketplace icon with a logic gate design.

## [1.0.2]

### Fixed
- Added an "FPGA" text label to the marketplace icon, matching the style of the sibling soldered-micropython-helper icon.

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
