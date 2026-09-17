/*
 * Config.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Config.hxx"
#include <cstdlib>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>

namespace embdevenv {

Config &Config::getInstance()
{
    static Config instance;
    return instance;
}

Config::Config()
    : _configPath("")
{
    // Default WebConfig
    _webConfig.enabled = true;
    _webConfig.port = 3886;
    _webConfig.bindAddress = "0.0.0.0";

    // Default McpConfig
    _mcpConfig.mode = "aimon";
    _mcpConfig.aimonHost = "127.0.0.1";
    _mcpConfig.aimonPort = 3885;
    _mcpConfig.standalonePort = 3889;
    _mcpConfig.reconnectIntervalSec = 5;

    // Default Targets
    TargetConfig pro;
    pro.name = "n1-655-pro";
    pro.model = "n1-655-pro";
    pro.description = "N1-655 Production Board";
    pro.enabled = true;
    pro.telnetPort = 3887;
    pro.consolePort = "/dev/ttyCH9344USB0";
    pro.consoleBaudRate = 115200;
    pro.mcuPort = "/dev/ttyCH9344USB3";
    pro.mcuBaudRate = 115200;
    pro.mcuPollIntervalMs = 2000;
    _targets.push_back(pro);

    TargetConfig devkit;
    devkit.name = "n1-655-devkit";
    devkit.model = "n1-655-devkit";
    devkit.description = "N1-655 Development Kit";
    devkit.enabled = true;
    devkit.telnetPort = 3888;
    devkit.consolePort = "/dev/ttyCH9344USB8";
    devkit.consoleBaudRate = 115200;
    devkit.mcuPort = "/dev/ttyCH9344USB11";
    devkit.mcuBaudRate = 115200;
    devkit.mcuPollIntervalMs = 2000;
    _targets.push_back(devkit);
}

std::string Config::resolveHomePath(const std::string &path)
{
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char *home = getenv("HOME");
    if (!home) {
        return path;
    }

    if (path == "~") {
        return std::string(home);
    }

    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }

    return path;
}

const std::string &Config::getConfigPath() const
{
    return _configPath;
}

const WebConfig &Config::getWebConfig() const
{
    return _webConfig;
}

const McpConfig &Config::getMcpConfig() const
{
    return _mcpConfig;
}

const std::vector<TargetConfig> &Config::getTargets() const
{
    return _targets;
}

void Config::setWebConfig(const WebConfig &web)
{
    _webConfig = web;
}

void Config::setMcpConfig(const McpConfig &mcp)
{
    _mcpConfig = mcp;
}

void Config::setTargets(const std::vector<TargetConfig> &targets)
{
    _targets = targets;
}

void Config::addTarget(const TargetConfig &target)
{
    _targets.push_back(target);
}

bool Config::load(const std::string &customPath)
{
    if (!customPath.empty()) {
        _configPath = resolveHomePath(customPath);
    } else {
        std::string homeDir = resolveHomePath("~/.config/embdevenv");
        mkdir(homeDir.c_str(), 0700);
        chmod(homeDir.c_str(), 0700);
        _configPath = homeDir + "/embdevenv.cfg";
    }

    libconfig::Config cfg;

    try {
        cfg.readFile(_configPath.c_str());
    } catch (const libconfig::FileIOException &) {
        // File does not exist yet, write default configuration
        return save();
    } catch (const libconfig::ParseException &pex) {
        fprintf(stderr, "[embdevenv] Config parse error at %s:%d - %s\n",
                pex.getFile(), pex.getLine(), pex.getError());
        return false;
    }

    // Web section
    if (cfg.exists("web")) {
        try {
            const libconfig::Setting &webSetting = cfg.lookup("web");
            webSetting.lookupValue("enabled", _webConfig.enabled);
            webSetting.lookupValue("port", _webConfig.port);
            webSetting.lookupValue("bind_address", _webConfig.bindAddress);
        } catch (const libconfig::SettingNotFoundException &) {
        }
    }

    // MCP section
    if (cfg.exists("mcp")) {
        try {
            const libconfig::Setting &mcpSetting = cfg.lookup("mcp");
            mcpSetting.lookupValue("mode", _mcpConfig.mode);
            mcpSetting.lookupValue("aimon_host", _mcpConfig.aimonHost);
            mcpSetting.lookupValue("aimon_port", _mcpConfig.aimonPort);
            mcpSetting.lookupValue("standalone_port", _mcpConfig.standalonePort);
            mcpSetting.lookupValue("reconnect_interval_sec", _mcpConfig.reconnectIntervalSec);
        } catch (const libconfig::SettingNotFoundException &) {
        }
    }

    // Targets section
    if (cfg.exists("targets")) {
        try {
            const libconfig::Setting &targetsSetting = cfg.lookup("targets");
            int count = targetsSetting.getLength();
            if (count > 0) {
                _targets.clear();
                for (int i = 0; i < count; ++i) {
                    const libconfig::Setting &t = targetsSetting[i];
                    TargetConfig tc;
                    t.lookupValue("name", tc.name);
                    t.lookupValue("model", tc.model);
                    t.lookupValue("description", tc.description);
                    t.lookupValue("enabled", tc.enabled);
                    t.lookupValue("telnet_port", tc.telnetPort);
                    t.lookupValue("console_port", tc.consolePort);
                    t.lookupValue("console_baud_rate", tc.consoleBaudRate);
                    t.lookupValue("mcu_port", tc.mcuPort);
                    t.lookupValue("mcu_baud_rate", tc.mcuBaudRate);
                    t.lookupValue("mcu_poll_interval_ms", tc.mcuPollIntervalMs);

                    if (!tc.name.empty()) {
                        _targets.push_back(tc);
                    }
                }
            }
        } catch (const libconfig::SettingNotFoundException &) {
        }
    }

    return true;
}

bool Config::save()
{
    if (_configPath.empty()) {
        std::string homeDir = resolveHomePath("~/.config/embdevenv");
        mkdir(homeDir.c_str(), 0700);
        chmod(homeDir.c_str(), 0700);
        _configPath = homeDir + "/embdevenv.cfg";
    }

    libconfig::Config cfg;
    libconfig::Setting &root = cfg.getRoot();

    // Web section
    libconfig::Setting &web = root.add("web", libconfig::Setting::TypeGroup);
    web.add("enabled", libconfig::Setting::TypeBoolean) = _webConfig.enabled;
    web.add("port", libconfig::Setting::TypeInt) = _webConfig.port;
    web.add("bind_address", libconfig::Setting::TypeString) = _webConfig.bindAddress;

    // MCP section
    libconfig::Setting &mcp = root.add("mcp", libconfig::Setting::TypeGroup);
    mcp.add("mode", libconfig::Setting::TypeString) = _mcpConfig.mode;
    mcp.add("aimon_host", libconfig::Setting::TypeString) = _mcpConfig.aimonHost;
    mcp.add("aimon_port", libconfig::Setting::TypeInt) = _mcpConfig.aimonPort;
    mcp.add("standalone_port", libconfig::Setting::TypeInt) = _mcpConfig.standalonePort;
    mcp.add("reconnect_interval_sec", libconfig::Setting::TypeInt) = _mcpConfig.reconnectIntervalSec;

    // Targets list
    libconfig::Setting &targets = root.add("targets", libconfig::Setting::TypeList);
    for (const auto &tc : _targets) {
        libconfig::Setting &t = targets.add(libconfig::Setting::TypeGroup);
        t.add("name", libconfig::Setting::TypeString) = tc.name;
        t.add("model", libconfig::Setting::TypeString) = tc.model;
        t.add("description", libconfig::Setting::TypeString) = tc.description;
        t.add("enabled", libconfig::Setting::TypeBoolean) = tc.enabled;
        t.add("telnet_port", libconfig::Setting::TypeInt) = tc.telnetPort;
        t.add("console_port", libconfig::Setting::TypeString) = tc.consolePort;
        t.add("console_baud_rate", libconfig::Setting::TypeInt) = tc.consoleBaudRate;
        t.add("mcu_port", libconfig::Setting::TypeString) = tc.mcuPort;
        t.add("mcu_baud_rate", libconfig::Setting::TypeInt) = tc.mcuBaudRate;
        t.add("mcu_poll_interval_ms", libconfig::Setting::TypeInt) = tc.mcuPollIntervalMs;
    }

    try {
        cfg.writeFile(_configPath.c_str());
        chmod(_configPath.c_str(), 0600);
        return true;
    } catch (const libconfig::FileIOException &) {
        fprintf(stderr, "[embdevenv] Failed to write config to %s\n", _configPath.c_str());
        return false;
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
