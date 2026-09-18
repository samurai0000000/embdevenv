# embdevenv - Design Document

## 1. Overview

`embdevenv` (Embedded Development Environment) is a high-performance C++ daemon that bridges host systems and remote development operators to embedded target boards. It manages hardware serial interfaces (UART consoles and management microcontrollers), provides continuous background telemetry collection, demultiplexes target serial consoles across multiple human operators via RFC 854 Telnet, serves an embedded web dashboard with real-time REST monitoring, and registers structured tool definitions with AI agents via Model Context Protocol (MCP).

### Key Capabilities
- **Multi-Board Simultaneous Management**: Concurrently connects to multiple target boards (e.g., development kits, production boards) within a single process.
- **Direct POSIX Serial Driver**: Native `termios` serial driver operating asynchronously with non-blocking I/O and zero dependency on GNU `screen`.
- **Target Microcontroller Management**: Autonomous background polling of MCU telemetry (voltage rails, temperature, fan speed, power states) and remote power control (Power ON, Power OFF, Reboot).
- **Console Demuxing via Telnet**: Circular line buffering for console history and RFC 854 Telnet server allowing multiple simultaneous operator attachments without multiplexing conflicts.
- **Embedded Web Dashboard & REST API**: Single-page modern HTML5/CSS3/JavaScript interface and JSON REST endpoints for target status inspection and control.
- **Dual MCP Subsystem**: Connects to an external AI gateway hub (such as `aimon` over TCP JSON-RPC 2.0) or operates as an autonomous standalone MCP server.

---

## 2. System Architecture

`embdevenv` is organized into five modular functional layers:

```
+-------------------------------------------------------------------------+
|                              Operators & AI                              |
|   Telnet Clients (RFC 854)   |   Web Browser / REST   |   AI MCP Agents |
+------------------------------+------------------------+-----------------+
               |                           |                     |
               v                           v                     v
+------------------------------+------------------------+-----------------+
|       Telnet Server          |       Web Server       | Tool Dispatcher |
|  (:3887, :3888 per target)   |     (:3886 REST/UI)    | (aimon / stdio) |
+------------------------------+------------------------+-----------------+
                                           |
                                           v
+-------------------------------------------------------------------------+
|                             Target Manager                               |
|        +-------------------------------------------------------+        |
|        |                   Target Instance                     |        |
|        |  +--------------------+       +--------------------+  |        |
|        |  |   Console Engine   |       |     MCU Engine     |  |        |
|        |  |  RingBuffer (10k)  |       | Telemetry (1k hist)|  |        |
|        |  +--------------------+       +--------------------+  |        |
|        |            |                            |             |        |
|        |            v                            v             |        |
|        |     SerialPort (UART)            SerialPort (MCU)     |        |
|        +-------------------------------------------------------+        |
+-------------------------------------------------------------------------+
                                           |
                                           v
+-------------------------------------------------------------------------+
|                         Hardware Target Boards                          |
|             /dev/ttyCH* (Console)   |   /dev/ttyCH* (MCU)               |
+-------------------------------------------------------------------------+
```

---

## 3. Class Hierarchy & Subsystems

The daemon is implemented under `namespace embdevenv`:

### 3.1 Serial & Data Storage Subsystems
- **`SerialPort`**: Encapsulates POSIX serial communication via file descriptors, termios raw-mode configuration (8N1, baud rates up to 921600, no flow control), non-blocking timed `poll()` reads, line extraction, thread-safe writes, and automatic reconnection.
- **`RingBuffer<T>`**: Thread-safe circular FIFO buffer protected by `std::mutex`, providing bounded in-memory retention and non-destructive window reads (`getLatest()`, `getAll()`).
- **`TelemetrySample` & `TelemetryStore`**: Structured data models capturing timestamped snapshots of power states, temperature, fan speed, and dynamic rail voltages, with JSON serialization and historical retention.

### 3.2 Target & MCU Abstraction
- **`Mcu`**: Abstract base class defining the hardware supervisor contract: `start()`, `stop()`, `powerOn()`, `powerOff()`, `reboot()`, `sendCommand()`, and telemetry retrieval.
- **`N1655Mcu`**: Specialized implementation for N1-655 series microcontrollers:
  - Command execution adhering to strict carriage return termination (`\r`) with prompt tracking (`# `).
  - Break/recovery via `\x03` (Ctrl-C) interrupts.
  - Background polling worker executing `bs` commands and parsing multi-line voltage, thermal, and fan metrics.
- **`Console`**: Manages the main serial console UART. Spawns an asynchronous ingestion thread buffering incoming lines into `RingBuffer<std::string>` and fanning out raw byte chunks to registered output listeners.
- **`TelnetSession` & `TelnetServer`**: RFC 854 network terminal server negotiating character mode (WILL ECHO, WILL SGA, DO BINARY) and bridging remote TCP sockets to the `Console` subsystem.
- **`Target`**: Base target container orchestrating a target's `Console`, `Mcu`, and `TelnetServer`.
- **`N1655Target`**: Concrete target specialization instantiating `N1655Mcu`, `Console`, and target-specific Telnet listeners.
- **`TargetFactory`**: Instantiates target subclasses based on model configuration strings (`n1-655-pro`, `n1-655-devkit`, etc.).
- **`TargetManager`**: Thread-safe registry and lifecycle manager holding all configured `Target` instances.

### 3.3 Network, Web & AI Gateway
- **`WebServer`**: Embedded HTTP service based on `httplib::Server` hosting the single-page web dashboard and REST API.
- **`ToolDispatcher`**: Central MCP dispatcher enumerating schemas for 18 tools and routing invocations to `TargetManager` and `Config`.
- **`AimonGatewayClient`**: Resilient TCP client maintaining an asynchronous JSON-RPC 2.0 session with the `aimon` gateway hub, handling capability registration and tool dispatch.
- **`StandaloneMcpServer`**: Autonomous TCP listener serving MCP JSON-RPC requests for standalone environments without an external gateway.
- **`Config`**: Singleton configuration parser and serializer built on `libconfig++`.

---

## 4. Concurrency & Threading Model

`embdevenv` is fundamentally multi-threaded, ensuring that long-running serial I/O or network communications never block other targets or management interfaces:

1. **Main Thread**: Initializes subsystems, parses command-line arguments, registers POSIX signal handlers (`SIGINT`, `SIGTERM`), and waits on a shutdown condition.
2. **Console Ingestion Threads (1 per Target)**: Dedicated worker polling the console serial port with `poll()`, writing to the line buffer, and fanning out bytes to active Telnet sessions.
3. **MCU Polling Threads (1 per Target)**: Dedicated worker executing periodic background telemetry queries, parsing hardware samples, and populating `TelemetryStore`.
4. **Telnet Server Threads**: An accept thread per target port and dedicated session workers for each connected human operator.
5. **Web Server Thread**: Dedicated worker servicing HTTP requests and REST API calls.
6. **MCP Client/Server Threads**: Background connection and I/O worker handling JSON-RPC message framing and dispatch.

Thread safety is maintained via granular `std::mutex` instances (`_cmdMutex` for serial port transactions, `_sessionsMutex` for client registries, and `_mutex` for ring buffer access).

---

## 5. Target Interfaces & Protocols

### 5.1 N1-655 MCU Command & Prompt Discipline
- **Baud Rate**: 115200 bps, 8 data bits, no parity, 1 stop bit (8N1).
- **Prompt**: Strict `# ` prompt.
- **Line Editing**: The target MCU firmware does not support backspace or ANSI escape sequences. Commands are issued cleanly with `\r` line termination.
- **Interrupt Handling**: Sending `\x03` (ASCII Ctrl-C) cancels pending commands or streaming output, restoring the prompt.
- **Telemetry Parsing**: The `bs` (Board Status) command yields multi-line telemetry blocks:
  ```text
  DCIN=12.03V, DCOUT=12.01V, SYS_5V=5.04V, SYS_3V3=3.31V, SYS_1V8=1.80V, AVDD85=0.85V
  VDDQ05=0.50V, VDDH105=1.05V, AVDD08=0.80V, AVDD12=1.20V, VDDi=0.85V, VDDi_DDR=0.85V
  TEMP=42.5C, FAN=2400RPM
  ```
  Voltages are mapped dynamically into a structured key-value table. Power state is inferred from `SYS_5V` (> 4.5V indicates powered ON).

---

## 6. Telnet Demux & Console Buffering

To solve the limitation where only one process can attach to a serial device at a time:
- The target console port is exclusively owned by `embdevenv::Console`.
- `TelnetServer` listens on a dedicated TCP port per target (e.g., 3887 for `n1-655-pro`, 3888 for `n1-655-devkit`).
- Multiple operators can connect simultaneously:
  ```bash
  telnet <host> 3887
  ```
- All connected operators receive console output in real-time. Any operator input is forwarded cleanly to the UART console.
- If no operator is connected, output continues buffering in the circular line buffer (up to 10,000 lines) viewable via the web interface or REST API.

---

## 7. Web Dashboard & REST API

The embedded web server serves both a responsive UI and REST endpoints:

### 7.1 REST Endpoints
| Method | Path | Description |
| :--- | :--- | :--- |
| `GET` | `/api/status` | Global daemon health and target summary |
| `GET` | `/api/targets` | List of all configured targets and statuses |
| `GET` | `/api/targets/:name` | Detailed status for a specific target |
| `GET` | `/api/targets/:name/mcu` | MCU connectivity and latest metrics |
| `GET` | `/api/targets/:name/mcu/telemetry` | Latest telemetry sample JSON |
| `GET` | `/api/targets/:name/mcu/telemetry/history?count=N` | Telemetry historical series |
| `POST` | `/api/targets/:name/mcu/power` | Power control: `{"action": "on"|"off"|"reboot"|"usb_download"}` |
| `POST` | `/api/targets/:name/mcu/command` | Raw MCU command: `{"command": "bs"}` |
| `GET` | `/api/targets/:name/console?lines=N` | Recent console log lines |
| `POST` | `/api/targets/:name/console/write` | Send input string to console |
| `POST` | `/api/targets/:name/console/clear` | Clear console history buffer |

---

## 8. MCP Server & Gateway Integration

`embdevenv` exposes 18 structured tools for AI agent interaction:

| MCP Tool Name | Description |
| :--- | :--- |
| `embdevenv_list_targets` | List all configured embedded targets and configurations |
| `embdevenv_get_target_status` | Comprehensive status of a specific target |
| `embdevenv_mcu_power_on` | Power on target hardware via MCU |
| `embdevenv_mcu_power_off` | Power off target hardware via MCU |
| `embdevenv_mcu_reboot` | Power cycle / reboot target board via MCU |
| `embdevenv_mcu_usb_download` | Put target board into USB download / boot mode for flashing |
| `embdevenv_mcu_get_power_state` | Query whether target is currently ON or OFF |
| `embdevenv_mcu_get_telemetry` | Retrieve latest telemetry snapshot |
| `embdevenv_mcu_get_voltages` | Retrieve breakdown of all voltage rails |
| `embdevenv_mcu_get_temperature` | Retrieve core thermal temperature in &deg;C |
| `embdevenv_mcu_get_fan_speed` | Retrieve cooling fan speed in RPM |
| `embdevenv_mcu_get_telemetry_history` | Retrieve historical telemetry samples |
| `embdevenv_mcu_send_command` | Execute arbitrary MCU command and return output |
| `embdevenv_console_read` | Read recent serial console buffer lines |
| `embdevenv_console_write` | Send command strings or keystrokes to console |
| `embdevenv_console_clear` | Clear console buffer memory |
| `embdevenv_telnet_get_status` | Get Telnet server port and active session count |
| `embdevenv_get_config` | Retrieve active daemon configuration |

---

## 9. Configuration Specification

Configuration is managed via `libconfig++` (default path: `~/.config/embdevenv/embdevenv.cfg`):

```libconfig
web: {
    enabled = true;
    port = 3886;
    bind_address = "0.0.0.0";
};

mcp: {
    mode = "aimon";             # "aimon" or "standalone"
    aimon_host = "127.0.0.1";
    aimon_port = 3885;
    standalone_port = 3889;
    reconnect_interval_sec = 5;
};

targets = (
    {
        name = "n1-655-pro";
        model = "n1-655-pro";
        description = "N1-655 Production Board";
        enabled = true;
        telnet_port = 3887;
        console_port = "/dev/ttyCH9344USB0";
        console_baud_rate = 115200;
        mcu_port = "/dev/ttyCH9344USB3";
        mcu_baud_rate = 115200;
        mcu_poll_interval_ms = 2000;
    },
    {
        name = "n1-655-devkit";
        model = "n1-655-devkit";
        description = "N1-655 Development Kit";
        enabled = true;
        telnet_port = 3888;
        console_port = "/dev/ttyCH9344USB8";
        console_baud_rate = 115200;
        mcu_port = "/dev/ttyCH9344USB11";
        mcu_baud_rate = 115200;
        mcu_poll_interval_ms = 2000;
    }
);
```
