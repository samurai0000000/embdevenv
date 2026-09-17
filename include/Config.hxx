/*
 * Config.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_CONFIG_HXX
#define EMBDEVENV_CONFIG_HXX

#include <string>
#include <vector>
#include <memory>
#include <libconfig.h++>
#include "TargetFactory.hxx"

namespace embdevenv {

struct WebConfig {
    bool enabled = true;
    int port = 3886;
    std::string bindAddress = "0.0.0.0";
};

struct McpConfig {
    std::string mode = "aimon"; // "aimon" or "standalone"
    std::string aimonHost = "127.0.0.1";
    int aimonPort = 3885;
    int standalonePort = 3889;
    int reconnectIntervalSec = 5;
};

class Config {
public:
    static Config &getInstance();

    bool load(const std::string &customPath = "");
    bool save();

    const std::string &getConfigPath() const;
    const WebConfig &getWebConfig() const;
    const McpConfig &getMcpConfig() const;
    const std::vector<TargetConfig> &getTargets() const;

    void setWebConfig(const WebConfig &web);
    void setMcpConfig(const McpConfig &mcp);
    void setTargets(const std::vector<TargetConfig> &targets);
    void addTarget(const TargetConfig &target);

    static std::string resolveHomePath(const std::string &path);

private:
    Config();
    ~Config() = default;
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    std::string _configPath;
    WebConfig _webConfig;
    McpConfig _mcpConfig;
    std::vector<TargetConfig> _targets;
};

} // namespace embdevenv

#endif // EMBDEVENV_CONFIG_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
