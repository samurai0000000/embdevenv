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
#include <sstream>
#include <thread>
#include <chrono>

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
            {"name", "embdevenv_mcu_power_on"},
            {"description", "Send power-on command to target microcontroller unit (MCU)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_power_off"},
            {"description", "Send power-off command to target microcontroller unit (MCU)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_reboot"},
            {"description", "Trigger hardware reboot sequence via target MCU."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_usb_download"},
            {"description", "Put target board into USB download / boot mode for firmware flashing via MCU command 'reset -usb -y'."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"double_reset", {{"type", "boolean"}, {"description", "Send command twice to avoid MCU state-latching (default: true)"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_power_state"},
            {"description", "Check whether target board is currently powered ON or OFF."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_telemetry"},
            {"description", "Retrieve latest telemetry snapshot from target MCU (all voltage rails, temperature, fan speed)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_voltages"},
            {"description", "Retrieve voltage rails breakdown (DCIN, DCOUT, SYS_5V, SYS_3V3, SYS_1V8, AVDD85, etc.)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_temperature"},
            {"description", "Retrieve current thermal reading (degrees Celsius) from target MCU."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
                }},
                {"required", json::array({"target"})}
            }}
        },
        {
            {"name", "embdevenv_mcu_get_fan_speed"},
            {"description", "Retrieve current cooling fan speed (RPM) from target MCU."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
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
            {"name", "embdevenv_mcu_send_command"},
            {"description", "Send a raw command to the target MCU and return the response."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"command", {{"type", "string"}, {"description", "Raw command string (e.g. 'bs', 'version', 'help')"}}},
                    {"timeout_ms", {{"type", "integer"}, {"description", "Timeout in milliseconds (default: 2000)"}}}
                }},
                {"required", json::array({"target", "command"})}
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
                    {"data", {{"type", "string"}, {"description", "Data or command to write"}}},
                    {"char_delay_ms", {{"type", "integer"}, {"description", "Inter-character pacing delay in ms to avoid UART buffer overflow (default: 0)"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target", "data"})}
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
            {"name", "embdevenv_console_start_log"},
            {"description", "Start continuous streaming capture of target serial console output to a file on host disk."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}},
                    {"file_path", {{"type", "string"}, {"description", "Host file path to write log into"}}},
                    {"console", {{"type", "string"}, {"description", "Console name (default: primary)"}}}
                }},
                {"required", json::array({"target", "file_path"})}
            }}
        },
        {
            {"name", "embdevenv_console_stop_log"},
            {"description", "Stop active serial console file logging for a target."},
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
            {"name", "embdevenv_telnet_get_status"},
            {"description", "Get Telnet server status, listening port, and active human session count."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
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
    } else if (name == "embdevenv_mcu_power_on") {
        return toolMcuPowerOn(target);
    } else if (name == "embdevenv_mcu_power_off") {
        return toolMcuPowerOff(target);
    } else if (name == "embdevenv_mcu_reboot") {
        return toolMcuReboot(target);
    } else if (name == "embdevenv_mcu_usb_download") {
        bool doubleReset = args.value("double_reset", true);
        return toolMcuUsbDownload(target, doubleReset);
    } else if (name == "embdevenv_mcu_get_power_state") {
        return toolMcuGetPowerState(target);
    } else if (name == "embdevenv_mcu_get_telemetry") {
        return toolMcuGetTelemetry(target);
    } else if (name == "embdevenv_mcu_get_voltages") {
        return toolMcuGetVoltages(target);
    } else if (name == "embdevenv_mcu_get_temperature") {
        return toolMcuGetTemperature(target);
    } else if (name == "embdevenv_mcu_get_fan_speed") {
        return toolMcuGetFanSpeed(target);
    } else if (name == "embdevenv_mcu_get_telemetry_history") {
        size_t count = args.value("count", 50);
        return toolMcuGetTelemetryHistory(target, count);
    } else if (name == "embdevenv_mcu_send_command") {
        std::string cmd = args.value("command", "");
        int timeoutMs = args.value("timeout_ms", 2000);
        return toolMcuSendCommand(target, cmd, timeoutMs);
    } else if (name == "embdevenv_console_read") {
        size_t lines = args.value("lines", 50);
        std::string console = args.value("console", "");
        return toolConsoleRead(target, lines, console);
    } else if (name == "embdevenv_console_write") {
        std::string data = args.value("data", "");
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
    } else if (name == "embdevenv_console_start_log") {
        std::string filePath = args.value("file_path", "");
        std::string console = args.value("console", "");
        return toolConsoleStartLog(target, filePath, console);
    } else if (name == "embdevenv_console_stop_log") {
        std::string console = args.value("console", "");
        return toolConsoleStopLog(target, console);
    } else if (name == "embdevenv_get_board_stats") {
        int windowDays = args.value("window_days", 30);
        return toolGetBoardStats(target, windowDays);
    } else if (name == "embdevenv_telnet_get_status") {
        return toolTelnetGetStatus(target);
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
    return TargetManager::getInstance().getStatusJson().dump(2);
}

std::string ToolDispatcher::toolGetTargetStatus(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target) {
        return "Error: Target '" + targetName + "' not found";
    }
    return target->getStatusJson().dump(2);
}

std::string ToolDispatcher::toolMcuPowerOn(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }
    bool ok = target->getMcu()->powerOn();
    json resp = {{"success", ok}, {"target", targetName}, {"action", "power_on"}};
    return resp.dump(2);
}

std::string ToolDispatcher::toolMcuPowerOff(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }
    bool ok = target->getMcu()->powerOff();
    json resp = {{"success", ok}, {"target", targetName}, {"action", "power_off"}};
    return resp.dump(2);
}

std::string ToolDispatcher::toolMcuReboot(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }
    bool ok = target->getMcu()->reboot();
    json resp = {{"success", ok}, {"target", targetName}, {"action", "reboot"}};
    return resp.dump(2);
}

std::string ToolDispatcher::toolMcuUsbDownload(const std::string &targetName, bool doubleReset)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }
    bool ok = target->getMcu()->resetUsbDownload(doubleReset);
    json resp = {
        {"success", ok},
        {"target", targetName},
        {"action", "usb_download"},
        {"double_reset", doubleReset}
    };
    return resp.dump(2);
}

std::string ToolDispatcher::toolMcuGetPowerState(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: Target or MCU not found for '" + targetName + "'";
    }
    json mcuJson = target->getMcu()->getStatusJson();
    bool powerOn = mcuJson.value("powerOn", false);
    json resp = {{"target", targetName}, {"powerOn", powerOn}};
    return resp.dump(2);
}

std::string ToolDispatcher::toolMcuGetTelemetry(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    return target->getMcu()->getTelemetryStore()->toJson(1).dump(2);
}

std::string ToolDispatcher::toolMcuGetVoltages(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    TelemetrySample sample;
    if (!target->getMcu()->getTelemetryStore()->getLatest(sample)) {
        return "Error: No telemetry samples recorded yet for '" + targetName + "'";
    }
    json j;
    j["target"] = targetName;
    j["timestampIso"] = sample.timestampIso;
    j["voltages"] = sample.voltages;
    return j.dump(2);
}

std::string ToolDispatcher::toolMcuGetTemperature(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    TelemetrySample sample;
    if (!target->getMcu()->getTelemetryStore()->getLatest(sample)) {
        return "Error: No telemetry samples recorded yet for '" + targetName + "'";
    }
    json j = {
        {"target", targetName},
        {"timestampIso", sample.timestampIso},
        {"temperatureC", sample.temperatureC}
    };
    return j.dump(2);
}

std::string ToolDispatcher::toolMcuGetFanSpeed(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    TelemetrySample sample;
    if (!target->getMcu()->getTelemetryStore()->getLatest(sample)) {
        return "Error: No telemetry samples recorded yet for '" + targetName + "'";
    }
    json j = {
        {"target", targetName},
        {"timestampIso", sample.timestampIso},
        {"fanRpm", sample.fanRpm}
    };
    return j.dump(2);
}

std::string ToolDispatcher::toolMcuGetTelemetryHistory(const std::string &targetName, size_t count)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
        return "Error: Telemetry not available for '" + targetName + "'";
    }
    return target->getMcu()->getTelemetryStore()->toJson(count).dump(2);
}

std::string ToolDispatcher::toolMcuSendCommand(const std::string &targetName, const std::string &cmd, int timeoutMs)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getMcu()) {
        return "Error: MCU not available for '" + targetName + "'";
    }
    std::string resp = target->getMcu()->sendCommand(cmd, timeoutMs);
    json j = {
        {"target", targetName},
        {"command", cmd},
        {"response", resp}
    };
    return j.dump(2);
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
    return j.dump(2);
}

std::string ToolDispatcher::toolConsoleWrite(const std::string &targetName, const std::string &data, int charDelayMs, const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Console not available for '" + targetName + "'";
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

    ssize_t written = console->writePaced(processed, charDelayMs);
    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", written >= 0},
        {"bytesWritten", written},
        {"charDelayMs", charDelayMs}
    };
    return j.dump(2);
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
    return j.dump(2);
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
    return j.dump(2);
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
    return j.dump(2);
}

std::string ToolDispatcher::toolConsoleStartLog(const std::string &targetName,
                                               const std::string &filePath,
                                               const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    std::string resolved = Config::resolveHomePath(filePath);
    bool ok = console->startLogFile(resolved);
    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", ok},
        {"filePath", resolved}
    };
    return j.dump(2);
}

std::string ToolDispatcher::toolConsoleStopLog(const std::string &targetName,
                                              const std::string &consoleName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    auto console = resolveTargetConsole(target, consoleName);
    if (!target || !console) {
        return "Error: Target or console not available for '" + targetName + "'";
    }

    console->stopLogFile();
    json j = {
        {"target", targetName},
        {"console", console->getName()},
        {"success", true},
        {"stopped", true}
    };
    return j.dump(2);
}

std::string ToolDispatcher::toolGetBoardStats(const std::string &targetName, int windowDays)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target) {
        return "Error: Target '" + targetName + "' not found";
    }

    json stats = UsageDatabase::getInstance().queryBoardStats(targetName, windowDays);
    return stats.dump(2);
}

std::string ToolDispatcher::toolTelnetGetStatus(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getTelnetServer()) {
        return "Error: Telnet server not available for '" + targetName + "'";
    }
    auto ts = target->getTelnetServer();
    json j = {
        {"target", targetName},
        {"port", ts->getPort()},
        {"activeSessions", ts->getActiveSessionCount()},
        {"running", ts->isRunning()}
    };
    return j.dump(2);
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

    return j.dump(2);
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
    return res.dump(2);
}

std::string ToolDispatcher::toolUsbListDevices()
{
    return FlashManager::getInstance().listUsbDevices().dump(2);
}

std::string ToolDispatcher::toolFlashGetStatus(const std::string &target)
{
    json status = FlashManager::getInstance().getLastResult(target).toJson();
    status["is_flashing"] = FlashManager::getInstance().isFlashing();
    return status.dump(2);
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
