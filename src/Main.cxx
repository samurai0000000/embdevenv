/*
 * Main.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Version.hxx"
#include "Config.hxx"
#include "TargetFactory.hxx"
#include "TargetManager.hxx"
#include "WebServer.hxx"
#include "AimonGatewayClient.hxx"
#include "StandaloneMcpServer.hxx"
#include <iostream>
#include <memory>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <cstring>

static std::atomic<bool> g_shutdown(false);

static void signalHandler(int sig)
{
    (void)sig;
    g_shutdown.store(true);
}

static void printUsage(const char *progName)
{
    std::cout << "Usage: " << progName << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c, --config <path>   Specify path to configuration file (default: ~/.config/embdevenv/embdevenv.cfg)\n"
              << "  -v, --version         Show version information and build metadata\n"
              << "  -h, --help            Show this help message\n"
              << std::endl;
}

static void printVersion()
{
    std::cout << "embdevenv " << EMBDEVENV_VERSION_STRING << "\n"
              << "Built: " << EMBDEVENV_DATE << "\n"
              << "Host: " << EMBDEVENV_HOSTNAME << " by " << EMBDEVENV_WHOAMI << "\n"
              << "Copyright (C) 2026, Charles Chiou\n"
              << std::endl;
}

int main(int argc, char **argv)
{
    std::string configPath = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                configPath = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a file path argument\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "[embdevenv] Starting embdevenv %s\n", EMBDEVENV_VERSION_STRING);

    // Setup signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // Load configuration
    auto &config = embdevenv::Config::getInstance();
    if (!config.load(configPath)) {
        fprintf(stderr, "[embdevenv] Warning: failed to load config, using defaults\n");
    } else {
        fprintf(stderr, "[embdevenv] Loaded configuration from %s\n", config.getConfigPath().c_str());
    }

    // Initialize Targets
    auto &targetMgr = embdevenv::TargetManager::getInstance();
    const auto &targetConfigs = config.getTargets();
    for (const auto &tc : targetConfigs) {
        auto target = embdevenv::TargetFactory::createTarget(tc);
        targetMgr.addTarget(target);
        fprintf(stderr, "[embdevenv] Configured target '%s' (%s)\n", tc.name.c_str(), tc.model.c_str());
    }

    // Start Web Server FIRST so monitoring interface is immediately accessible
    std::unique_ptr<embdevenv::WebServer> webServer;
    const auto &webCfg = config.getWebConfig();
    if (webCfg.enabled) {
        webServer = std::make_unique<embdevenv::WebServer>(webCfg.port, webCfg.bindAddress);
        if (!webServer->start()) {
            fprintf(stderr, "[embdevenv] Warning: failed to start web server on port %d\n", webCfg.port);
        }
    }

    // Start Targets
    targetMgr.startAll();

    // Start MCP Subsystem
    const auto &mcpCfg = config.getMcpConfig();
    std::unique_ptr<embdevenv::StandaloneMcpServer> standaloneMcp;

    if (mcpCfg.mode == "aimon") {
        fprintf(stderr, "[embdevenv] MCP mode: aimon gateway (%s:%d)\n",
                mcpCfg.aimonHost.c_str(), mcpCfg.aimonPort);
        embdevenv::AimonGatewayClient::getInstance().start(mcpCfg.aimonHost, mcpCfg.aimonPort);
    } else if (mcpCfg.mode == "standalone") {
        fprintf(stderr, "[embdevenv] MCP mode: standalone server (port %d)\n",
                mcpCfg.standalonePort);
        standaloneMcp = std::make_unique<embdevenv::StandaloneMcpServer>(mcpCfg.standalonePort);
        standaloneMcp->start();
    } else {
        fprintf(stderr, "[embdevenv] MCP mode '%s' not recognized, skipping MCP start\n",
                mcpCfg.mode.c_str());
    }

    fprintf(stderr, "[embdevenv] Daemon initialization complete. Running...\n");

    // Main event loop: wait for shutdown signal
    while (!g_shutdown.load()) {
        sleep(1);
    }

    fprintf(stderr, "\n[embdevenv] Shutdown signal received, terminating gracefully...\n");

    // Stop MCP subsystem
    if (mcpCfg.mode == "aimon") {
        embdevenv::AimonGatewayClient::getInstance().stop();
        embdevenv::AimonGatewayClient::getInstance().join();
    } else if (standaloneMcp != nullptr) {
        standaloneMcp->stop();
    }

    // Stop Web Server
    if (webServer != nullptr) {
        webServer->stop();
    }

    // Stop Targets
    targetMgr.stopAll();

    fprintf(stderr, "[embdevenv] Clean shutdown complete.\n");
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
