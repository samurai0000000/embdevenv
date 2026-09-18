/*
 * ToolDispatcher.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ToolDispatcher.hxx"
#include "TargetManager.hxx"
#include "Config.hxx"
#include <sstream>

namespace embdevenv {

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
                    {"lines", {{"type", "integer"}, {"description", "Maximum lines to read (default: 50)"}}}
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
                    {"data", {{"type", "string"}, {"description", "Data or command to write"}}}
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
                    {"target", {{"type", "string"}, {"description", "Target name"}}}
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
        return toolConsoleRead(target, lines);
    } else if (name == "embdevenv_console_write") {
        std::string data = args.value("data", "");
        return toolConsoleWrite(target, data);
    } else if (name == "embdevenv_console_clear") {
        return toolConsoleClear(target);
    } else if (name == "embdevenv_telnet_get_status") {
        return toolTelnetGetStatus(target);
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

std::string ToolDispatcher::toolConsoleRead(const std::string &targetName, size_t lines)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getConsole()) {
        return "Error: Console not available for '" + targetName + "'";
    }
    std::vector<std::string> logLines = target->getConsole()->getRecentLines(lines);
    json j;
    j["target"] = targetName;
    j["lineCount"] = logLines.size();
    j["lines"] = logLines;
    return j.dump(2);
}

std::string ToolDispatcher::toolConsoleWrite(const std::string &targetName, const std::string &data)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getConsole()) {
        return "Error: Console not available for '" + targetName + "'";
    }
    ssize_t written = target->getConsole()->write(data);
    json j = {
        {"target", targetName},
        {"success", written >= 0},
        {"bytesWritten", written}
    };
    return j.dump(2);
}

std::string ToolDispatcher::toolConsoleClear(const std::string &targetName)
{
    auto target = TargetManager::getInstance().getTarget(targetName);
    if (!target || !target->getConsole()) {
        return "Error: Console not available for '" + targetName + "'";
    }
    target->getConsole()->clearBuffer();
    json j = {{"target", targetName}, {"success", true}, {"cleared", true}};
    return j.dump(2);
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
            {"telnetPort", t.telnetPort},
            {"consolePort", t.consolePort},
            {"consoleBaudRate", t.consoleBaudRate},
            {"mcuPort", t.mcuPort},
            {"mcuBaudRate", t.mcuBaudRate},
            {"mcuPollIntervalMs", t.mcuPollIntervalMs}
        });
    }
    j["targets"] = targetsArr;

    return j.dump(2);
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
