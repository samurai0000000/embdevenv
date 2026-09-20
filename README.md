# embdevenv

**Embedded Development Environment** (`embdevenv`) is a daemon designed to connect to, monitor, and control embedded target hardware remotely. It provides direct POSIX serial communication with target consoles and management microcontrollers (MCUs), demultiplexes target serial consoles across multiple simultaneous operators via RFC 854 Telnet, serves an embedded real-time web dashboard and REST API, and integrates with AI agents via the Model Context Protocol (MCP).

## Features

- **Multi-Target Hardware Management**: Concurrently manage multiple embedded target boards (development kits, production units) within a single daemon process.
- **Direct POSIX Serial Driver**: Native, non-blocking asynchronous serial I/O using POSIX `termios` (no external dependency on GNU `screen`).
- **Telemetry Ingestion & Supervisor**: Background polling of target microcontrollers to capture multi-rail voltages, system temperatures, cooling fan speeds, and board power states.
- **Telnet Console Demultiplexing**: Multiple human operators or automation scripts can connect concurrently to target UART consoles via dedicated Telnet ports.
- **Modern Web Dashboard**: Responsive dark-mode single-page web interface with real-time target switching, live telemetry visualization, power control, and interactive terminal output.
- **Target Firmware Flashing**: Embedded `usb-matrix` utility with MCU USB boot orchestration for automated board recovery and firmware flashing directly over USB.
- **Dual MCP Integration**: Seamlessly exports 16 structured tools to the `aimon` gateway hub over TCP JSON-RPC 2.0, or operates as a standalone MCP server.
- **Structured Configuration**: Complete configuration management powered by `libconfig++`.

## Getting Started

### Prerequisites

- A C++17 compliant compiler (`g++` >= 9.0 or `clang++` >= 10.0)
- `cmake` (>= 3.16)
- `make`
- `pkg-config`
- `libconfig++` (`libconfig++-dev`)
- POSIX threads (`pthread`)

### Building

The project uses a top-level `Makefile` wrapping CMake configuration and compilation:

```bash
# Clone repository with submodules
git clone --recurse-submodules <repository-url>
cd embdevenv

# Build the daemon
make

# Clean build artifacts
make clean

# Purge build directory for pristine rebuild
make distclean
```

The compiled binary will be produced at `build/embdevenv`.

### Running

```bash
# Run with default configuration (~/.config/embdevenv/embdevenv.cfg)
./build/embdevenv

# Run with a custom configuration file
./build/embdevenv -c /path/to/embdevenv.cfg

# Show version and build metadata
./build/embdevenv --version

# Show help options
./build/embdevenv --help
```

### Accessing Interfaces

- **Web Dashboard**: Open `http://localhost:3886` in any browser.
- **Telnet Console**: Connect directly using `telnet localhost <telnet_port>` (e.g. `telnet localhost 3887`).
- **REST API**: Query `http://localhost:3886/api/status`.
- **MCP Gateway**: Connected automatically to `127.0.0.1:3885` when configured in `aimon` mode.

## Documentation

For full architectural specifications, class hierarchies, concurrency models, and MCP tool schemas, see [Design.md](Design.md).

## License & Copyright

Copyright (C) 2026, Charles Chiou. All rights reserved.