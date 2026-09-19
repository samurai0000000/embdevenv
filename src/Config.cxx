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

    // No hardcoded targets: targets are loaded dynamically from config file
    _targets.clear();
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

const FlashingGlobalConfig &Config::getFlashingGlobalConfig() const
{
    return _flashingConfig;
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

    // Global flashing section
    if (cfg.exists("flashing")) {
        try {
            const libconfig::Setting &fSetting = cfg.lookup("flashing");
            std::string bin;
            if (fSetting.lookupValue("usb_matrix_bin", bin)) {
                _flashingConfig.usbMatrixBin = resolveHomePath(bin);
            }
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
                    t.lookupValue("usb_path", tc.usbPath);

                    // Consoles list
                    if (t.exists("consoles")) {
                        const libconfig::Setting &consolesSetting = t.lookup("consoles");
                        int cCount = consolesSetting.getLength();
                        for (int c = 0; c < cCount; ++c) {
                            const libconfig::Setting &cs = consolesSetting[c];
                            ConsoleConfig cc;
                            cs.lookupValue("name", cc.name);
                            if (!cs.lookupValue("device", cc.port)) {
                                cs.lookupValue("port", cc.port);
                            }
                            cs.lookupValue("baud_rate", cc.baudRate);
                            cs.lookupValue("telnet_port", cc.telnetPort);
                            cs.lookupValue("port_index", cc.portIndex);
                            int bufLines = 10000;
                            if (cs.lookupValue("buffer_lines", bufLines)) {
                                cc.lineBufferCapacity = static_cast<size_t>(bufLines);
                            }
                            if (!cc.name.empty() || !cc.port.empty() || cc.portIndex >= 0) {
                                tc.consoles.push_back(cc);
                            }
                        }
                    }

                    // Legacy flat console fields
                    t.lookupValue("telnet_port", tc.telnetPort);
                    t.lookupValue("console_port", tc.consolePort);
                    t.lookupValue("console_baud_rate", tc.consoleBaudRate);

                    // MCU group
                    if (t.exists("mcu")) {
                        const libconfig::Setting &mcuSetting = t.lookup("mcu");
                        if (!mcuSetting.lookupValue("device", tc.mcu.port)) {
                            mcuSetting.lookupValue("port", tc.mcu.port);
                        }
                        mcuSetting.lookupValue("baud_rate", tc.mcu.baudRate);
                        mcuSetting.lookupValue("prompt", tc.mcu.prompt);
                        mcuSetting.lookupValue("telnet_port", tc.mcu.telnetPort);
                        mcuSetting.lookupValue("poll_interval_ms", tc.mcu.pollIntervalMs);
                        mcuSetting.lookupValue("port_index", tc.mcu.portIndex);
                    }

                    // Flashing group
                    if (t.exists("flashing")) {
                        const libconfig::Setting &fs = t.lookup("flashing");
                        fs.lookupValue("enabled", tc.flashing.enabled);
                        fs.lookupValue("chip", tc.flashing.chip);
                        std::string ustPath, bldPath, fwPath;
                        if (fs.lookupValue("ust_image", ustPath)) {
                            tc.flashing.ustImage = resolveHomePath(ustPath);
                        }
                        if (fs.lookupValue("bld_image", bldPath)) {
                            tc.flashing.bldImage = resolveHomePath(bldPath);
                        }
                        if (fs.lookupValue("default_fw_image", fwPath) || fs.lookupValue("fw_image", fwPath)) {
                            tc.flashing.defaultFwImage = resolveHomePath(fwPath);
                        }
                    }

                    // Legacy flat mcu fields
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
        if (!tc.usbPath.empty()) {
            t.add("usb_path", libconfig::Setting::TypeString) = tc.usbPath;
        }

        // MCU group
        if (!tc.mcu.port.empty() || tc.mcu.portIndex >= 0) {
            libconfig::Setting &mcu = t.add("mcu", libconfig::Setting::TypeGroup);
            if (!tc.mcu.port.empty()) {
                mcu.add("device", libconfig::Setting::TypeString) = tc.mcu.port;
            }
            if (tc.mcu.portIndex >= 0) {
                mcu.add("port_index", libconfig::Setting::TypeInt) = tc.mcu.portIndex;
            }
            mcu.add("baud_rate", libconfig::Setting::TypeInt) = tc.mcu.baudRate;
            mcu.add("prompt", libconfig::Setting::TypeString) = tc.mcu.prompt;
            mcu.add("telnet_port", libconfig::Setting::TypeInt) = tc.mcu.telnetPort;
            mcu.add("poll_interval_ms", libconfig::Setting::TypeInt) = tc.mcu.pollIntervalMs;
        } else if (!tc.mcuPort.empty()) {
            t.add("mcu_port", libconfig::Setting::TypeString) = tc.mcuPort;
            t.add("mcu_baud_rate", libconfig::Setting::TypeInt) = tc.mcuBaudRate;
            t.add("mcu_poll_interval_ms", libconfig::Setting::TypeInt) = tc.mcuPollIntervalMs;
        }

        // Consoles list
        if (!tc.consoles.empty()) {
            libconfig::Setting &consoles = t.add("consoles", libconfig::Setting::TypeList);
            for (const auto &cc : tc.consoles) {
                libconfig::Setting &c = consoles.add(libconfig::Setting::TypeGroup);
                c.add("name", libconfig::Setting::TypeString) = cc.name;
                if (!cc.port.empty()) {
                    c.add("device", libconfig::Setting::TypeString) = cc.port;
                }
                if (cc.portIndex >= 0) {
                    c.add("port_index", libconfig::Setting::TypeInt) = cc.portIndex;
                }
                c.add("baud_rate", libconfig::Setting::TypeInt) = cc.baudRate;
                c.add("telnet_port", libconfig::Setting::TypeInt) = cc.telnetPort;
                c.add("buffer_lines", libconfig::Setting::TypeInt) = static_cast<int>(cc.lineBufferCapacity);
            }
        } else if (!tc.consolePort.empty()) {
            t.add("telnet_port", libconfig::Setting::TypeInt) = tc.telnetPort;
            t.add("console_port", libconfig::Setting::TypeString) = tc.consolePort;
            t.add("console_baud_rate", libconfig::Setting::TypeInt) = tc.consoleBaudRate;
        }

        // Flashing group
        if (tc.flashing.enabled || !tc.flashing.chip.empty() || !tc.flashing.defaultFwImage.empty()) {
            libconfig::Setting &fl = t.add("flashing", libconfig::Setting::TypeGroup);
            fl.add("enabled", libconfig::Setting::TypeBoolean) = tc.flashing.enabled;
            if (!tc.flashing.chip.empty()) {
                fl.add("chip", libconfig::Setting::TypeString) = tc.flashing.chip;
            }
            if (!tc.flashing.ustImage.empty()) {
                fl.add("ust_image", libconfig::Setting::TypeString) = tc.flashing.ustImage;
            }
            if (!tc.flashing.bldImage.empty()) {
                fl.add("bld_image", libconfig::Setting::TypeString) = tc.flashing.bldImage;
            }
            if (!tc.flashing.defaultFwImage.empty()) {
                fl.add("default_fw_image", libconfig::Setting::TypeString) = tc.flashing.defaultFwImage;
            }
        }
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
