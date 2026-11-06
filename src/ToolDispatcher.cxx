/*
 * ToolDispatcher.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ToolDispatcher.hxx"
#include "TargetManager.hxx"
#include "Config.hxx"
#include "FlashManager.hxx"
#include "UsageDatabase.hxx"
#include "TerminalSanitizer.hxx"
#include "Version.hxx"
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sys/stat.h>

namespace embdevenv {

static std::shared_ptr<Console> resolveTargetConsole(std::shared_ptr<Target> target, const std::string &consoleName)
{
    if (!target) return nullptr;
    if (!consoleName.empty()) {
        auto c = target->getConsoleByName(consoleName);
        if (c) return c;
    }
    return target->getConsole(0);
}

using json = nlohmann::json;

json ToolDispatcher::getToolDefinitions()
{
    return json::array({
        {
            {"name", "embdevenv_list_targets"},
            {"description", "List all configured embedded hardware targets, their models, enabled state, and ports."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "embdevenv_get_target_status"},
            {"description", "Get comprehensive operational status for a specific embedded target."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name (e.g. 'n1-655-pro')"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_power"},
            {"description", "Manage target board power states via microcontroller unit (MCU): power on, power off, reboot, or USB download mode."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"action", {
                        {"type", "string"},
                        {"enum", {"power_on", "power_off", "reboot", "usb_download"}},
                        {"description", "Power action to execute: 'power_on', 'power_off', 'reboot', or 'usb_download'"}
                    }},
                    {"double_reset", {{"type", "boolean"}, {"description", "Send reset twice in usb_download to avoid state latching (default: true)"}}}
                }},
                {"required", json::array({"target", "action"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_telemetry"},
            {"description", "Retrieve latest telemetry snapshot or specific metric from target MCU (voltages, temperature, fan speed, power state)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"metric", {
                        {"type", "string"},
                        {"enum", {"all", "voltages", "thermal", "fan", "power"}},
                        {"description", "Telemetry metric to retrieve: 'all' (default), 'voltages', 'thermal', 'fan', or 'power'"}
                    }}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_telemetry_history"},
            {"description", "Retrieve historical telemetry samples over time for target."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"count", {{"type", "integer"}, {"description", "Number of historical samples (default: 50)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },

        {
            {"name", "embdevenv_console_read"},
            {"description", "Read recent buffered output lines from the target's serial console."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"lines", {{"type", "integer"}, {"description", "Maximum lines to read (default: 50)"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_console_write"},
            {"description", "Send keystrokes or command lines into target's serial console."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"data", {{"type", "string"}, {"description", "Data or command to write (aliases: 'input', 'command', 'text')"}}},
                    {"input", {{"type", "string"}, {"description", "Alias for 'data'"}}},
                    {"command", {{"type", "string"}, {"description", "Alias for 'data'"}}},
                    {"text", {{"type", "string"}, {"description", "Alias for 'data'"}}},
                    {"char_delay_ms", {{"type", "integer"}, {"description", "Inter-character pacing delay in ms to avoid UART buffer overflow (default: 0)"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_console_clear"},
            {"description", "Clear the in-memory serial console log buffer for a target."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_console_catch_bootloader"},
            {"description", "Interrupt target bootloader autoboot sequence within milliseconds and stop at boot prompt (e.g. =>). Native C++ replacement for catch_uboot socket scripts."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"prompt", {{"type", "string"}, {"description", "Bootloader prompt to match (default: '=>')"}}},
                    {"interrupt_key", {{"type", "string"}, {"description", "Character to send repeatedly (default: ' ')"}}},
                    {"timeout_sec", {{"type", "integer"}, {"description", "Maximum seconds to wait (default: 30)"}}},
                    {"reboot_first", {{"type", "boolean"}, {"description", "Reboot target via MCU before starting catch loop (default: false)"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_console_expect"},
            {"description", "Send command with optional character pacing and wait for one or more sentinel string patterns or timeout. Returns matched pattern and captured output. Native C++ replacement for pexpect / socket capture scripts."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"wait_for", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Array of string patterns to wait for (e.g. ['=>', 'Kernel panic', 'login:'])"}
                    }},
                    {"command", {{"type", "string"}, {"description", "Optional command line to send before waiting"}}},
                    {"timeout_sec", {{"type", "integer"}, {"description", "Maximum seconds to wait (default: 60)"}}},
                    {"char_delay_ms", {{"type", "integer"}, {"description", "Pacing delay between characters in ms (default: 10)"}}},
                    {"log_file", {{"type", "string"}, {"description", "Optional host file path to record streaming console output"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target", "wait_for"})}
            }}
        },
        {
            {"name", "embdevenv_console_log"},
            {"description", "Manage continuous serial console file logging on host disk (start or stop capture)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"action", {
                        {"type", "string"},
                        {"enum", {"start", "stop"}},
                        {"description", "Logging action: 'start' or 'stop'"}
                    }},
                    {"file_path", {{"type", "string"}, {"description", "Host file path to write log into (required for 'start')"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target", "action"})}
            }}
        },
        {
            {"name", "embdevenv_console_load"},
            {"description", "Load binary or image file into target memory over serial console using YMODEM (loady) or XMODEM (loadx) protocol. Native replacement for sx/sb/lrzsz."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target board name (e.g. 'n1-655-pro')"}}},
                    {"file_path", {{"type", "string"}, {"description", "Host filesystem path to binary or image file to transfer"}}},
                    {"protocol", {{"type", "string"}, {"description", "Transfer protocol: 'loady' (YMODEM, default) or 'loadx' (XMODEM)"}, {"enum", json::array({"loady", "loadx", "ymodem", "xmodem"})}}},
                    {"address", {{"type", "string"}, {"description", "Target memory address (e.g. '0x20000000'). Appended to bootloader load command."}}},
                    {"send_command", {{"type", "boolean"}, {"description", "If true (default), sends bootloader command (e.g. 'loady 0x20000000') and waits for handshake. If false, assumes target is already waiting."}}},
                    {"timeout_sec", {{"type", "integer"}, {"description", "Overall transfer timeout in seconds (default: 300)"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target", "file_path"})}
            }}
        },
        {
            {"name", "embdevenv_get_board_stats"},
            {"description", "Retrieve persistent board usage statistics, wear metrics, flasher history, reboot counts, and 1-year rolling averages from SQLite database."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"window_days", {{"type", "integer"}, {"description", "Rolling window in days (default: 30, e.g. 1, 7, 30, 365)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_get_config"},
            {"description", "Retrieve current embdevenv daemon configuration and settings."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "embdevenv_flash_target"},
            {"description", "Start flashing firmware to a target asynchronously. Use embdevenv_flash_get_status to monitor progress."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name (e.g. 'n1-655-pro')"}}},
                    {"chip", {{"type", "string"}, {"description", "Chip architecture (e.g. 'cv3ad655', optional, defaults to target config)"}}},
                    {"ust_image", {{"type", "string"}, {"description", "Path to UST binary (optional, defaults to target config)"}}},
                    {"bld_image", {{"type", "string"}, {"description", "Path to BLD binary (optional, defaults to target config)"}}},
                    {"fw_image", {{"type", "string"}, {"description", "Path to firmware package binary (optional, defaults to target config)"}}},
                    {"force_usb_boot", {{"type", "boolean"}, {"description", "Trigger MCU reset -usb -y before flashing (default: true)"}}},
                    {"timeout_sec", {{"type", "integer"}, {"description", "Flashing timeout in seconds (default: 60)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_usb_list_devices"},
            {"description", "Scan host USB buses for connected targets and recovery devices via usb-matrix -l."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "embdevenv_flash_get_status"},
            {"description", "Get status and log output of the most recent firmware flashing operation for a target."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        }
    });
}

std::string ToolDispatcher::dispatchTool(const std::string &name, const json &args)
{
    if (name == "embdevenv_list_targets") {
        return toolListTargets();
    }

    if (name == "embdevenv_get_config") {
        return toolGetConfig();
    }

    if (name == "embdevenv_usb_list_devices") {
        return toolUsbListDevices();
    }

    std::string target = args.value("target", "");
    if (target.empty()) {
        return "Error: missing required argument 'target'";
    }

    if (name == "embdevenv_get_target_status") {
        return toolGetTargetStatus(target);
    } else if (name == "embdevenv_mcu_power") {
        std::string action = args.value("action", "");
        bool doubleReset = args.value("double_reset", true);
        return toolMcuPower(target, action, doubleReset);
    } else if (name == "embdevenv_mcu_get_telemetry") {
        std::string metric = args.value("metric", "all");
        return toolMcuGetTelemetry(target, metric);
    } else if (name == "embdevenv_mcu_get_telemetry_history") {
        size_t count = args.value("count", 50);
        return toolMcuGetTelemetryHistory(target, count);
    } else if (name == "embdevenv_console_read") {
        size_t lines = args.value("lines", 50);
        std::string console = args.value("console", "");
        return toolConsoleRead(target, lines, console);
    } else if (name == "embdevenv_console_write") {
        std::string data = args.value("data", "");
        if (data.empty() && args.contains("input") && args["input"].is_string()) {
            data = args["input"].get<std::string>();
        }
        if (data.empty() && args.contains("command") && args["command"].is_string()) {
            data = args["command"].get<std::string>();
        }
        if (data.empty() && args.contains("text") && args["text"].is_string()) {
            data = args["text"].get<std::string>();
        }
        int charDelayMs = args.value("char_delay_ms", 0);
        std::string console = args.value("console", "");
        return toolConsoleWrite(target, data, charDelayMs, console);
    } else if (name == "embdevenv_console_clear") {
        std::string console = args.value("console", "");
        return toolConsoleClear(target, console);
    } else if (name == "embdevenv_console_catch_bootloader") {
        std::string prompt = args.value("prompt", "=>");
        std::string interruptKey = args.value("interrupt_key", " ");
        int timeoutSec = args.value("timeout_sec", 30);
        bool rebootFirst = args.value("reboot_first", false);
        std::string console = args.value("console", "");
        return toolConsoleCatchBootloader(target, prompt, interruptKey, timeoutSec, rebootFirst, console);
    } else if (name == "embdevenv_console_expect") {
        std::string command = args.value("command", "");
        if (command.empty() && args.contains("input") && args["input"].is_string()) {
            command = args["input"].get<std::string>();
        }
        if (command.empty() && args.contains("data") && args["data"].is_string()) {
            command = args["data"].get<std::string>();
        }
        std::vector<std::string> patterns;
        if (args.contains("wait_for") && args["wait_for"].is_array()) {
            for (const auto &p : args["wait_for"]) {
                if (p.is_string()) {
                    patterns.push_back(p.get<std::string>());
                }
            }
        }
        int timeoutSec = args.value("timeout_sec", 60);
        int charDelayMs = args.value("char_delay_ms", 10);
        std::string logFile = args.value("log_file", "");
        std::string console = args.value("console", "");
        return toolConsoleExpect(target, command, patterns, timeoutSec, charDelayMs, logFile, console);
    } else if (name == "embdevenv_console_log") {
        std::string action = args.value("action", "");
        std::string filePath = args.value("file_path", "");
        std::string console = args.value("console", "");
        return toolConsoleLog(target, action, filePath, console);
    } else if (name == "embdevenv_console_load") {
        std::string filePath = args.value("file_path", "");
        if (filePath.empty() && args.contains("file") && args["file"].is_string()) {
            filePath = args["file"].get<std::string>();
        }
        if (filePath.empty() && args.contains("path") && args["path"].is_string()) {
            filePath = args["path"].get<std::string>();
        }
        std::string protocol = args.value("protocol", "loady");
        std::string address = args.value("address", "");
        bool sendCommand = args.value("send_command", true);
        int timeoutSec = args.value("timeout_sec", 300);
        std::string console = args.value("console", "");
        return toolConsoleLoad(target, filePath, protocol, address, sendCommand, timeoutSec, console);
    } else if (name == "embdevenv_get_board_stats") {
        int windowDays = args.value("window_days", 30);
        return toolGetBoardStats(target, windowDays);
    } else if (name == "embdevenv_flash_target") {
        std::string chip = args.value("chip", "");
        std::string ustImage = args.value("ust_image", "");
        std::string bldImage = args.value("bld_image", "");
        std::string fwImage = args.value("fw_image", "");
        bool forceUsbBoot = args.value("force_usb_boot", true);
        int timeoutSec = args.value("timeout_sec", 60);
        return toolFlashTarget(target, chip, ustImage, bldImage, fwImage, forceUsbBoot, timeoutSec);
    } else if (name == "embdevenv_flash_get_status") {
        return toolFlashGetStatus(target);
    }

    return "Error: Unknown tool " + name;
}

std::string ToolDispatcher::toolListTargets()
{
    return TargetManager::getInstance().getStatusJson().dump();
}

std::string ToolDispatcher::toolGetTargetStatus(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target) {
        return "Error: Target '" + targetName + "' not found";
    }
    return target->getStatusJson().dump();
}

std::string ToolDispatcher::toolMcuPower(const std::string &targetName,
                                        const std::string &action,
                                        bool doubleReset)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }

    bool ok = false;
    if (action == "power_on") {
        ok = target->getMcu()->powerOn();
    } else if (action == "power_off") {
        ok = target->getMcu()->powerOff();
    } else if (action == "reboot") {
        ok = target->getMcu()->reboot();
    } else if (action == "usb_download") {
        ok = target->getMcu()->resetUsbDownload(doubleReset);
    } else {
        return "Error: Unknown power action '" + action + "'. Valid actions: power_on, power_off, reboot, usb_download";
    }

    json resp = {
        {"success", ok},
        {"target", targetName},
        {"action", action}
    };
    if (action == "usb_download") {
        resp["double_reset"] = doubleReset;
    }
    return resp.dump();
}

std::string ToolDispatcher::toolMcuGetTelemetry(const std::string &targetName,
                                               const std::string &metric)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }

    TelemetrySample sample;
    bool hasSample = target->getMcu()->getTelemetryStore()->getLatest(sample);

    if (metric == "voltages") {
        if (!hasSample) {
            return "Error: No telemetry samples recorded yet for '" + targetName + "'";
        }
        json j = {
            {"target", targetName},
            {"timestampIso", sample.timestampIso},
            {"voltages", sample.voltages}
        };
        return j.dump();
    } else if (metric == "thermal") {
        if (!hasSample) {
            return "Error: No telemetry samples recorded yet for '" + targetName + "'";
        }
        json j = {
            {"target", targetName},
            {"timestampIso", sample.timestampIso},
            {"temperatureC", sample.temperatureC}
        };
        return j.dump();
    } else if (metric == "fan") {
        if (!hasSample) {
            return "Error: No telemetry samples recorded yet for '" + targetName + "'";
        }
        json j = {
            {"target", targetName},
            {"timestampIso", sample.timestampIso},
            {"fanRpm", sample.fanRpm}
        };
        return j.dump();
    } else if (metric == "power") {
        json mcuJson = target->getMcu()->getStatusJson();
        bool powerOn = mcuJson.value("powerOn", false);
        bool connected = mcuJson.value("connected", false);
        json resp = {
            {"target", targetName},
            {"powerOn", powerOn},
            {"connected", connected}
        };
        return resp.dump();
    } else if (metric == "all" || metric.empty()) {
        return target->getMcu()->getTelemetryStore()->toJson(1).dump();
    }

    return "Error: Unknown metric '" + metric + "'. Valid metrics: all, voltages, thermal, fan, power";
}

std::string ToolDispatcher::toolMcuGetTelemetryHistory(const std::string &targetName, size_t count)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    return target->getMcu()->getTelemetryStore()->toJson(count).dump();
}

std::string ToolDispatcher::toolConsoleRead(const std::string &targetName, size_t lines, const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Console not available for '" + targetName + "'";
    }
    std::vector<std::string> logLines = console->getRecentLines(lines);
    json j;
    j["target"] = targetName;
    j["console"] = console->getName();
    j["lineCount"] = logLines.size();
    j["lines"] = logLines;
    return j.dump();
}

std::string ToolDispatcher::toolConsoleWrite(const std::string &targetName, const std::string &data, int charDelayMs, const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Console not available for '" + targetName + "'";
    }

    if (console->isTransferInProgress()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "Console is currently locked for active serial file transfer"}
        };
        return j.dump();
    }

    if (data.empty()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"bytesWritten", 0},
            {"error", "No data provided to write. Please specify 'data' or 'input' parameter."}
        };
        return j.dump();
    }

    std::string processed = data;
    
    // Unescape literal slash-n and slash-r in case the LLM sent escaped string representations
    size_t pos = 0;
    while ((pos = processed.find("\\n", pos)) != std::string::npos) {
        processed.replace(pos, 2, "\n");
        pos += 1;
    }
    pos = 0;
    while ((pos = processed.find("\\r", pos)) != std::string::npos) {
        processed.replace(pos, 2, "\r");
        pos += 1;
    }

    // Normalize Windows-style \r\n bytes to standard \n to prevent double-enter execution in U-Boot
    pos = 0;
    while ((pos = processed.find("\r\n", pos)) != std::string::npos) {
        processed.replace(pos, 2, "\n");
        pos += 1;
    }

    // Filter automated terminal feedback (CPR, DA, DSR) if present in raw input
    processed = TerminalSanitizer::sanitize(processed);

    ssize_t written = console->writePaced(processed, charDelayMs);
    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", written >= 0},
        {"bytesWritten", written},
        {"charDelayMs", charDelayMs}
    };
    return j.dump();
}

std::string ToolDispatcher::toolConsoleClear(const std::string &targetName, const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Console not available for '" + targetName + "'";
    }
    console->clearBuffer();
    json j = {{"target", targetName}, {"console", console->getName()}, {"success", true}, {"cleared", true}};
    return j.dump();
}

std::string ToolDispatcher::toolConsoleCatchBootloader(const std::string &targetName,
                                                      const std::string &prompt,
                                                      const std::string &interruptKey,
                                                      int timeoutSec,
                                                      bool rebootFirst,
                                                      const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    if (console->isTransferInProgress()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "Console is currently locked for active serial file transfer"}
        };
        return j.dump();
    }

    if (rebootFirst && target->getMcu()) {
        target->getMcu()->reboot();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto res = console->catchBootloader(prompt, interruptKey, timeoutSec);
    UsageDatabase::getInstance().recordEvent(
        targetName,
        "bootloader_catch",
        res.success ? "success" : "timeout",
        static_cast<int64_t>(res.elapsedSec * 1000.0),
        res.matchedPrompt
    );

    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", res.success},
        {"matchedPrompt", res.matchedPrompt},
        {"elapsedSec", res.elapsedSec},
        {"capturedOutput", res.capturedOutput}
    };
    return j.dump();
}

std::string ToolDispatcher::toolConsoleExpect(const std::string &targetName,
                                             const std::string &command,
                                             const std::vector<std::string> &patterns,
                                             int timeoutSec,
                                             int charDelayMs,
                                             const std::string &logFilePath,
                                             const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    if (console->isTransferInProgress()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "Console is currently locked for active serial file transfer"}
        };
        return j.dump();
    }

    std::string resolvedLog = !logFilePath.empty() ? Config::resolveHomePath(logFilePath) : "";
    auto res = console->expect(command, patterns, timeoutSec, charDelayMs, resolvedLog);

    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", res.success},
        {"matchedPattern", res.matchedPattern},
        {"elapsedSec", res.elapsedSec},
        {"capturedOutput", res.capturedOutput}
    };
    if (!resolvedLog.empty()) {
        j["logFile"] = resolvedLog;
    }
    return j.dump();
}

std::string ToolDispatcher::toolConsoleLog(const std::string &targetName,
                                          const std::string &action,
                                          const std::string &filePath,
                                          const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    if (action == "start") {
        if (console->isTransferInProgress()) {
            json j = {
                {"target", targetName},
                {"console", console->getName()},
                {"success", false},
                {"error", "Console is currently locked for active serial file transfer"}
            };
            return j.dump();
        }

        if (filePath.empty()) {
            return "Error: 'file_path' argument is required when action is 'start'";
        }
        std::string resolved = Config::resolveHomePath(filePath);
        bool ok = console->startLogFile(resolved);
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", ok},
            {"action", "start"},
            {"filePath", resolved}
        };
        return j.dump();
    } else if (action == "stop") {
        console->stopLogFile();
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", true},
            {"action", "stop"}
        };
        return j.dump();
    }

    return "Error: Unknown logging action '" + action + "'. Valid actions: start, stop";
}

std::string ToolDispatcher::toolConsoleLoad(const std::string &targetName,
                                           const std::string &filePath,
                                           const std::string &protocol,
                                           const std::string &address,
                                           bool sendCommand,
                                           int timeoutSec,
                                           const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    if (filePath.empty()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "'file_path' argument is required"}
        };
        return j.dump();
    }

    if (console->isTransferInProgress()) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "Console is currently locked for active serial file transfer"}
        };
        return j.dump();
    }

    std::string resolvedPath = Config::resolveHomePath(filePath);
    struct stat st;
    if (stat(resolvedPath.c_str(), &st) != 0) {
        json j = {
            {"target", targetName},
            {"console", console->getName()},
            {"success", false},
            {"error", "File not found: " + resolvedPath}
        };
        return j.dump();
    }

    XyModem::Protocol proto = XyModem::Protocol::YMODEM;
    std::string protoLower = protocol;
    std::transform(protoLower.begin(), protoLower.end(), protoLower.begin(), ::tolower);
    if (protoLower == "loadx" || protoLower == "xmodem") {
        proto = XyModem::Protocol::XMODEM;
    }

    auto res = console->transferFile(resolvedPath, proto, sendCommand, address, timeoutSec);

    UsageDatabase::getInstance().recordEvent(
        targetName,
        proto == XyModem::Protocol::YMODEM ? "console_loady" : "console_loadx",
        res.success ? "success" : "failed",
        static_cast<int64_t>(res.elapsedSec * 1000.0),
        res.errorMessage.empty() ? (std::to_string(res.bytesSent) + " bytes") : res.errorMessage
    );

    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", res.success},
        {"protocol", res.protocol},
        {"fileSize", res.fileSize},
        {"bytesSent", res.bytesSent},
        {"packetsSent", res.packetsSent},
        {"retries", res.retries},
        {"elapsedSec", res.elapsedSec},
        {"transferRateKBps", res.transferRateKBps},
        {"capturedOutput", res.capturedOutput}
    };
    if (!res.success && !res.errorMessage.empty()) {
        j["errorMessage"] = res.errorMessage;
    }
    return j.dump();
}

std::string ToolDispatcher::toolGetBoardStats(const std::string &targetName, int windowDays)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target) {
        return "Error: Target '" + targetName + "' not found";
    }

    json stats = UsageDatabase::getInstance().queryBoardStats(targetName, windowDays);
    return stats.dump();
}

std::string ToolDispatcher::toolGetConfig()
{
    const auto &cfg = Config::getInstance();
    json j;
    j["configPath"] = cfg.getConfigPath();
    j["web"] = {
        {"enabled", cfg.getWebConfig().enabled},
        {"port", cfg.getWebConfig().port},
        {"bindAddress", cfg.getWebConfig().bindAddress}
    };
    j["mcp"] = {
        {"mode", cfg.getMcpConfig().mode},
        {"aimonHost", cfg.getMcpConfig().aimonHost},
        {"aimonPort", cfg.getMcpConfig().aimonPort},
        {"standalonePort", cfg.getMcpConfig().standalonePort}
    };

    json targetsArr = json::array();
    for (const auto &t : cfg.getTargets()) {
        targetsArr.push_back({
            {"name", t.name},
            {"model", t.model},
            {"description", t.description},
            {"enabled", t.enabled},
            {"usbPath", t.usbPath},
            {"telnetPort", t.telnetPort},
            {"consolePort", t.consolePort},
            {"consoleBaudRate", t.consoleBaudRate},
            {"mcuPort", t.mcuPort},
            {"mcuBaudRate", t.mcuBaudRate},
            {"mcuPollIntervalMs", t.mcuPollIntervalMs},
            {"flashing", {
                {"chip", t.flashing.chip},
                {"ustImage", t.flashing.ustImage},
                {"bldImage", t.flashing.bldImage},
                {"defaultFwImage", t.flashing.defaultFwImage}
            }}
        });
    }
    j["targets"] = targetsArr;
    j["flashing"] = {
        {"usbMatrixBin", cfg.getFlashingGlobalConfig().usbMatrixBin}
    };

    return j.dump();
}

std::string ToolDispatcher::toolFlashTarget(const std::string &target,
                                           const std::string &chip,
                                           const std::string &ustImage,
                                           const std::string &bldImage,
                                           const std::string &fwImage,
                                           bool forceUsbBoot,
                                           int timeoutSec)
{
    FlashParams params;
    params.target = target;
    params.chip = chip;
    params.ustImage = ustImage;
    params.bldImage = bldImage;
    params.fwImage = fwImage;
    params.forceUsbBoot = forceUsbBoot;
    params.timeoutSec = timeoutSec;

    // Check if flashing is already in progress to avoid silent drops
    if (FlashManager::getInstance().isFlashing()) {
        return "{\"success\": false, \"stage\": \"rejected\", \"error\": \"Another flashing operation is currently in progress.\"}";
    }

    std::thread([params]() {
        FlashManager::getInstance().flashTarget(params);
    }).detach();

    json res = {
        {"success", true},
        {"stage", "started"},
        {"target", params.target},
        {"message", "Firmware flash initiated in the background. Use embdevenv_flash_get_status to monitor progress."}
    };
    return res.dump();
}

std::string ToolDispatcher::toolUsbListDevices()
{
    return FlashManager::getInstance().listUsbDevices().dump();
}

std::string ToolDispatcher::toolFlashGetStatus(const std::string &target)
{
    json status = FlashManager::getInstance().getLastResult(target).toJson();
    status["is_flashing"] = FlashManager::getInstance().isFlashing();
    return status.dump();
}

static json handleSingleJsonRpc(const json &req, const std::string &defaultProtocolVersion)
{
    if (!req.is_object()) {
        return json{
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {
                {"code", -32600},
                {"message", "Invalid Request"}
            }}
        };
    }

    std::string method = req.value("method", "");
    bool isNotification = false;
    if (!req.contains("id")) {
        isNotification = true;
    } else if (req["id"].is_null() && method.rfind("notifications/", 0) == 0) {
        isNotification = true;
    } else if (method.rfind("notifications/", 0) == 0) {
        isNotification = true;
    }

    if (isNotification) {
        return json();
    }

    auto reqId = req["id"];

    if (method == "initialize") {
        std::string clientVersion;
        if (req.contains("params") && req["params"].is_object()) {
            clientVersion = req["params"].value("protocolVersion", "");
        }
        std::string chosenVersion = defaultProtocolVersion;
        if (clientVersion == "2024-11-05" || clientVersion == "2025-03-26" || clientVersion == "2025-11-25") {
            chosenVersion = clientVersion;
        }

        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", {
                {"protocolVersion", chosenVersion},
                {"capabilities", {
                    {"tools", json::object()},
                    {"resources", json::object()},
                    {"prompts", json::object()}
                }},
                {"serverInfo", {
                    {"name", "embdevenv"},
                    {"version", EMBDEVENV_VERSION_STRING}
                }}
            }}
        };
    } else if (method == "ping") {
        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", json::object()}
        };
    } else if (method == "tools/list") {
        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", {
                {"tools", ToolDispatcher::getToolDefinitions()}
            }}
        };
    } else if (method == "resources/list") {
        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", {
                {"resources", json::array()}
            }}
        };
    } else if (method == "prompts/list") {
        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", {
                {"prompts", json::array()}
            }}
        };
    } else if (method == "tools/call") {
        json params = req.value("params", json::object());
        std::string toolName = params.value("name", "");
        json args = params.value("arguments", json::object());

        std::string resultText = ToolDispatcher::dispatchTool(toolName, args);

        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"result", {
                {"content", json::array({
                    {
                        {"type", "text"},
                        {"text", resultText}
                    }
                })}
            }}
        };
    } else {
        return json{
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"error", {
                {"code", -32601},
                {"message", "Method not found: " + method}
            }}
        };
    }
}

std::string ToolDispatcher::handleJsonRpc(const std::string &rawBody, const std::string &defaultProtocolVersion)
{
    json parsed;
    try {
        parsed = json::parse(rawBody);
    } catch (...) {
        json parseErr = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {
                {"code", -32700},
                {"message", "Parse error"}
            }}
        };
        return parseErr.dump();
    }

    if (parsed.is_array()) {
        if (parsed.empty()) {
            json invReq = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {
                    {"code", -32600},
                    {"message", "Invalid Request"}
                }}
            };
            return invReq.dump();
        }

        json respArray = json::array();
        for (const auto &item : parsed) {
            json itemResp = handleSingleJsonRpc(item, defaultProtocolVersion);
            if (!itemResp.is_null()) {
                respArray.push_back(itemResp);
            }
        }

        if (respArray.empty()) {
            return "";
        }
        return respArray.dump();
    } else if (parsed.is_object()) {
        json singleResp = handleSingleJsonRpc(parsed, defaultProtocolVersion);
        if (singleResp.is_null()) {
            return "";
        }
        return singleResp.dump();
    } else {
        json invReq = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {
                {"code", -32600},
                {"message", "Invalid Request"}
            }}
        };
        return invReq.dump();
    }
}

} // namespace embdevenv

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
