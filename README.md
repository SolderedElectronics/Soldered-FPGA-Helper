# FPGA Helper by Soldered

A Visual Studio Code extension for developing FPGA projects, with first-class support for Soldered boards. It wraps the [apio](https://github.com/FPGAwars/apio) toolchain and board-specific tooling behind a single panel, so you can go from a blank folder to a bitstream on your board without touching a terminal.

> **Note:** This extension is experimental and under active development. Currently only the **TinyFPGA MINI1** board is supported.

## Features

- **Project scaffolding** — generate a working example (blinky) into an empty folder, create a new project elsewhere, or start from one of the bundled tutorial examples.
- **Build & Upload** — synthesize your design and program it onto the board directly from the panel.
- **Simulation & Test** — simulate a testbench and view the waveform in GTKWave, or run testbenches headlessly and get a pass/fail report.
- **Automatic board detection** — the board port list updates as you plug in or unplug devices over USB.
- **Managed toolchain** — apio is installed into an isolated environment on first use and kept up to date automatically; you don't need to install or manage it yourself.

## Requirements

- **Python 3.9+** on your `PATH` — used to set up the isolated apio environment. Nothing else needs to be installed manually; apio and its build tools are downloaded automatically on first use.

## Getting started

1. Install the extension and open the **FPGA Helper** panel from the activity bar.
2. Under **Project**, either generate the example project into an empty folder or create a new project in a new location.
3. Under **Board**, select your board and its port. If it doesn't show up, hold the board's button while plugging it in to enter bootloader mode — the boot LED pulses while it's active.
4. Under **Build & Upload**, click **Build & Upload** to program the board.

Use **Simulation & Test** once your project has a testbench file (named `*_tb.v`).

## License

MIT — see [LICENSE](LICENSE).
