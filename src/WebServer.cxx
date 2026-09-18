/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "TargetManager.hxx"
#include "FlashManager.hxx"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <sstream>

namespace embdevenv {

WebServer::WebServer(int port, const std::string &bindAddress)
    : _port(port)
    , _bindAddress(bindAddress)
    , _running(false)
{
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start()
{
    if (_running) {
        return true;
    }

    _server = std::make_unique<httplib::Server>();
    setupRoutes();

    _running = true;
    _serverThread = std::thread(&WebServer::serverWorker, this);

    _server->wait_until_ready();

    fprintf(stderr, "[embdevenv] WebServer listening at http://%s:%d\n",
            _bindAddress.c_str(), _port);
    return true;
}

void WebServer::stop()
{
    if (!_running) {
        return;
    }

    _running = false;

    if (_server != nullptr) {
        _server->stop();
    }

    if (_serverThread.joinable()) {
        _serverThread.join();
    }

    _server.reset();
    fprintf(stderr, "[embdevenv] WebServer stopped\n");
}

bool WebServer::isRunning() const
{
    return _running;
}

int WebServer::getPort() const
{
    return _port;
}

const std::string &WebServer::getBindAddress() const
{
    return _bindAddress;
}

void WebServer::serverWorker()
{
    if (_server != nullptr) {
        if (!_server->listen(_bindAddress.c_str(), _port)) {
            fprintf(stderr, "[embdevenv] WebServer error: failed to listen on %s:%d\n",
                    _bindAddress.c_str(), _port);
        }
    }
    _running = false;
}

void WebServer::setupRoutes()
{
    if (_server == nullptr) {
        return;
    }

    // Root dashboard
    _server->Get("/", [this](const httplib::Request &, httplib::Response &res) {
        res.set_content(getDashboardHtml(), "text/html");
    });

    // Overall status
    _server->Get("/api/status", [](const httplib::Request &, httplib::Response &res) {
        nlohmann::json j;
        j["status"] = "ok";
        j["manager"] = TargetManager::getInstance().getStatusJson();
        res.set_content(j.dump(2), "application/json");
    });

    // Targets list
    _server->Get("/api/targets", [](const httplib::Request &, httplib::Response &res) {
        nlohmann::json j = TargetManager::getInstance().getStatusJson();
        res.set_content(j.dump(2), "application/json");
    });

    // Target detail
    _server->Get(R"(/api/targets/([^/]+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }
        res.set_content(target->getStatusJson().dump(2), "application/json");
    });

    // Target MCU status
    _server->Get(R"(/api/targets/([^/]+)/mcu)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getMcu()) {
            res.status = 404;
            res.set_content(R"({"error":"MCU not found for target"})", "application/json");
            return;
        }
        res.set_content(target->getMcu()->getStatusJson().dump(2), "application/json");
    });

    // Target MCU latest telemetry
    _server->Get(R"(/api/targets/([^/]+)/mcu/telemetry)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
            res.status = 404;
            res.set_content(R"({"error":"Telemetry store not found"})", "application/json");
            return;
        }
        res.set_content(target->getMcu()->getTelemetryStore()->toJson(1).dump(2), "application/json");
    });

    // Target MCU telemetry history
    _server->Get(R"(/api/targets/([^/]+)/mcu/telemetry/history)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getMcu() || !target->getMcu()->getTelemetryStore()) {
            res.status = 404;
            res.set_content(R"({"error":"Telemetry store not found"})", "application/json");
            return;
        }
        size_t count = 100;
        if (req.has_param("count")) {
            try {
                count = std::stoul(req.get_param_value("count"));
            } catch (...) {
            }
        }
        res.set_content(target->getMcu()->getTelemetryStore()->toJson(count).dump(2), "application/json");
    });

    // Target MCU power control
    _server->Post(R"(/api/targets/([^/]+)/mcu/power)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getMcu()) {
            res.status = 404;
            res.set_content(R"({"error":"Target or MCU not found"})", "application/json");
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string action = body.value("action", "");
            bool success = false;
            if (action == "on") {
                success = target->getMcu()->powerOn();
            } else if (action == "off") {
                success = target->getMcu()->powerOff();
            } else if (action == "reboot") {
                success = target->getMcu()->reboot();
            } else if (action == "usb_download" || action == "usb_boot") {
                bool doubleReset = body.value("double_reset", true);
                success = target->getMcu()->resetUsbDownload(doubleReset);
            } else {
                res.status = 400;
                res.set_content(R"({"error":"Invalid action, use 'on', 'off', 'reboot', or 'usb_download'"})", "application/json");
                return;
            }

            nlohmann::json resp;
            resp["success"] = success;
            resp["action"] = action;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 400;
            nlohmann::json err;
            err["error"] = ex.what();
            res.set_content(err.dump(), "application/json");
        }
    });

    // Target MCU custom command
    _server->Post(R"(/api/targets/([^/]+)/mcu/command)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getMcu()) {
            res.status = 404;
            res.set_content(R"({"error":"Target or MCU not found"})", "application/json");
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string cmd = body.value("command", "");
            if (cmd.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"Empty command"})", "application/json");
                return;
            }

            int timeoutMs = body.value("timeout_ms", 2000);
            std::string output = target->getMcu()->sendCommand(cmd, timeoutMs);

            nlohmann::json resp;
            resp["success"] = true;
            resp["command"] = cmd;
            resp["response"] = output;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 400;
            nlohmann::json err;
            err["error"] = ex.what();
            res.set_content(err.dump(), "application/json");
        }
    });

    // Target Console lines
    _server->Get(R"(/api/targets/([^/]+)/console)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }

        std::shared_ptr<Console> console;
        if (req.has_param("console")) {
            console = target->getConsoleByName(req.get_param_value("console"));
        }
        if (!console) {
            console = target->getConsole();
        }

        if (!console) {
            res.status = 404;
            res.set_content(R"({"error":"Console not found for target"})", "application/json");
            return;
        }

        size_t lines = 100;
        if (req.has_param("lines")) {
            try {
                lines = std::stoul(req.get_param_value("lines"));
            } catch (...) {
            }
        }

        nlohmann::json j;
        j["name"] = console->getName();
        j["port"] = console->getPort();
        j["baud_rate"] = console->getBaudRate();
        j["telnet_port"] = console->getTelnetPort();
        j["lines"] = console->getRecentLines(lines);
        j["connected"] = console->isConnected();
        res.set_content(j.dump(2), "application/json");
    });

    // Write to console
    _server->Post(R"(/api/targets/([^/]+)/console/write)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string consoleName = body.value("console", "");
            std::shared_ptr<Console> console;
            if (!consoleName.empty()) {
                console = target->getConsoleByName(consoleName);
            }
            if (!console) {
                console = target->getConsole();
            }

            if (!console) {
                res.status = 404;
                res.set_content(R"({"error":"Console not found for target"})", "application/json");
                return;
            }

            std::string data = body.value("data", "");
            // In raw serial mode (cfmakeraw), Linux terminal shells expect carriage return (\r)
            // as the line delimiter (Enter). If data ends with \n without \r, convert it.
            if (!data.empty() && data.back() == '\n') {
                if (data.size() < 2 || data[data.size() - 2] != '\r') {
                    data.pop_back();
                    data += "\r";
                }
            } else if (data.empty()) {
                // Empty string with submit or Enter keypress sends carriage return
                data = "\r";
            }

            ssize_t written = console->write(data);
            nlohmann::json resp;
            resp["success"] = (written >= 0);
            resp["written"] = written;
            resp["console"] = console->getName();
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 400;
            nlohmann::json err;
            err["error"] = ex.what();
            res.set_content(err.dump(), "application/json");
        }
    });

    // Clear console buffer
    _server->Post(R"(/api/targets/([^/]+)/console/clear)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }

        std::shared_ptr<Console> console;
        if (req.has_param("console")) {
            console = target->getConsoleByName(req.get_param_value("console"));
        }
        if (!console) {
            console = target->getConsole();
        }

        if (!console) {
            res.status = 404;
            res.set_content(R"({"error":"Console not found for target"})", "application/json");
            return;
        }

        console->clearBuffer();
        res.set_content(R"({"success":true})", "application/json");
    });

    // List host USB devices via usb-matrix -l
    _server->Get("/api/usb/devices", [](const httplib::Request &, httplib::Response &res) {
        auto j = FlashManager::getInstance().listUsbDevices();
        res.set_content(j.dump(2), "application/json");
    });

    // Flash target firmware
    _server->Post(R"(/api/targets/([^/]+)/flash)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }

        try {
            FlashParams params;
            params.target = name;
            if (!req.body.empty()) {
                auto body = nlohmann::json::parse(req.body);
                params.chip = body.value("chip", "");
                params.ustImage = body.value("ust_image", "");
                params.bldImage = body.value("bld_image", "");
                params.fwImage = body.value("fw_image", "");
                params.forceUsbBoot = body.value("force_usb_boot", true);
                params.timeoutSec = body.value("timeout_sec", 60);
            }

            FlashResult result = FlashManager::getInstance().flashTarget(params);
            res.status = result.success ? 200 : 500;
            res.set_content(result.toJson().dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 400;
            nlohmann::json err;
            err["error"] = ex.what();
            res.set_content(err.dump(), "application/json");
        }
    });

    // Flashing status for target
    _server->Get(R"(/api/targets/([^/]+)/flash/status)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto result = FlashManager::getInstance().getLastResult(name).toJson();
        result["is_flashing"] = FlashManager::getInstance().isFlashing();
        res.set_content(result.dump(2), "application/json");
    });

    // WebSocket endpoint for interactive bidirectional serial console stream
    _server->WebSocket(R"(/ws/targets/([^/]+)/console)", [](const httplib::Request &req, httplib::ws::WebSocket &ws) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            ws.close();
            return;
        }

        std::shared_ptr<Console> console;
        if (req.has_param("console")) {
            console = target->getConsoleByName(req.get_param_value("console"));
        }
        if (!console) {
            console = target->getConsole();
        }
        if (!console) {
            ws.close();
            return;
        }

        // Hydrate initial terminal state with recent buffer history
        auto recent = console->getRecentLines(100);
        for (const auto &line : recent) {
            ws.send(line + "\r\n");
        }

        // Stream live serial UART output directly to WebSocket
        auto wsSendMutex = std::make_shared<std::mutex>();
        int listenerId = console->addListener([&ws, wsSendMutex](const char *data, size_t len) {
            std::lock_guard<std::mutex> lock(*wsSendMutex);
            if (ws.is_open()) {
                ws.send(std::string(data, len));
            }
        });

        // Set timeout to 0 (infinite wait for incoming client keystrokes)
        ws.set_read_timeout(0);

        std::string msg;
        while (true) {
            auto res = ws.read(msg);
            if (res == httplib::ws::ReadResult::Fail) {
                break;
            }
            if (res == httplib::ws::ReadResult::Timeout) {
                continue;
            }
            if (!msg.empty()) {
                console->write(msg);
            }
        }

        console->removeListener(listenerId);
    });
}

std::string WebServer::getDashboardHtml() const
{
    return R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>embdevenv - Embedded Target Dashboard</title>
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.min.css">
    <script src="https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.min.js"></script>
    <style>
        :root {
            --bg-base: #0a0f1d;
            --bg-card: #151d30;
            --bg-card-hover: #1c263f;
            --bg-term: #050811;
            --border: #232f48;
            --text-main: #f1f5f9;
            --text-muted: #94a3b8;
            --accent-cyan: #06b6d4;
            --accent-emerald: #10b981;
            --accent-amber: #f59e0b;
            --accent-rose: #f43f5e;
            --accent-indigo: #6366f1;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            background-color: var(--bg-base);
            color: var(--text-main);
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
        }
        header {
            background: linear-gradient(180deg, #111827 0%, rgba(17,24,39,0.8) 100%);
            border-bottom: 1px solid var(--border);
            padding: 0.6rem 1.5rem;
            display: flex;
            align-items: center;
            justify-content: space-between;
        }
        .brand {
            display: flex;
            align-items: center;
            gap: 0.75rem;
        }
        .brand-logo {
            width: 32px;
            height: 32px;
            background: linear-gradient(135deg, var(--accent-cyan), var(--accent-indigo));
            border-radius: 8px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-weight: bold;
            font-size: 1.1rem;
            color: white;
            box-shadow: 0 0 12px rgba(6,182,212,0.4);
        }
        .brand-title {
            font-size: 1.25rem;
            font-weight: 700;
            letter-spacing: -0.025em;
        }
        .brand-sub {
            font-size: 0.8rem;
            color: var(--text-muted);
            margin-left: 0.5rem;
            background: #1f293d;
            padding: 0.15rem 0.5rem;
            border-radius: 4px;
        }
        .header-meta {
            display: flex;
            align-items: center;
            gap: 1.5rem;
            font-size: 0.85rem;
            color: var(--text-muted);
        }
        .status-pill {
            display: inline-flex;
            align-items: center;
            gap: 0.4rem;
            padding: 0.25rem 0.75rem;
            border-radius: 9999px;
            font-weight: 500;
            background: rgba(16,185,129,0.15);
            color: var(--accent-emerald);
            border: 1px solid rgba(16,185,129,0.3);
        }
        .status-dot {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: currentColor;
            animation: pulse 2s infinite;
        }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
        main {
            flex: 1;
            padding: 0.75rem 1.5rem;
            max-width: 1280px;
            width: 100%;
            margin: 0 auto;
            display: flex;
            flex-direction: column;
            gap: 0.75rem;
        }
        .tabs-container {
            display: flex;
            gap: 0.5rem;
            border-bottom: 1px solid var(--border);
            padding-bottom: 0.35rem;
            overflow-x: auto;
        }
        .tab-btn {
            background: transparent;
            border: 1px solid transparent;
            color: var(--text-muted);
            padding: 0.35rem 0.85rem;
            font-size: 0.85rem;
            font-weight: 600;
            border-radius: 6px;
            cursor: pointer;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 0.4rem;
            white-space: nowrap;
        }
        .tab-btn:hover {
            color: var(--text-main);
            background: var(--bg-card);
        }
        .tab-btn.active {
            color: var(--accent-cyan);
            background: var(--bg-card);
            border-color: var(--border);
            box-shadow: 0 4px 12px rgba(0,0,0,0.2);
        }
        .dashboard-grid {
            display: grid;
            grid-template-columns: 320px 1fr;
            gap: 1rem;
            align-items: start;
        }
        .console-card {
            background: var(--bg-card);
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 0.75rem 1rem;
            display: flex;
            flex-direction: column;
            gap: 0.5rem;
            box-shadow: 0 4px 20px rgba(0,0,0,0.15);
            min-height: 460px;
            box-sizing: border-box;
            transition: all 0.2s ease;
        }
        .console-card.fullscreen {
            position: fixed;
            top: 0;
            left: 0;
            width: 100vw;
            height: 100vh;
            max-height: 100vh;
            z-index: 10000;
            border-radius: 0;
            margin: 0;
            border: none;
            background: var(--bg-card);
            padding: 0.5rem 0.75rem;
            display: flex;
            flex-direction: column;
            overflow: hidden;
            box-sizing: border-box;
        }
        .console-card.fullscreen .card-header {
            flex-shrink: 0;
        }
        .card {
            background: var(--bg-card);
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 0.75rem 1rem;
            display: flex;
            flex-direction: column;
            gap: 0.55rem;
            box-shadow: 0 4px 20px rgba(0,0,0,0.15);
            box-sizing: border-box;
        }
        .card-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            border-bottom: 1px solid var(--border);
            padding-bottom: 0.5rem;
            gap: 0.5rem;
        }
        .card-title {
            font-size: 0.95rem;
            font-weight: 600;
            color: var(--text-main);
            display: flex;
            align-items: center;
            gap: 0.4rem;
        }
        .btn-group {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 0.4rem;
        }
        .btn {
            padding: 0.35rem 0.5rem;
            border-radius: 6px;
            font-size: 0.8rem;
            font-weight: 600;
            border: none;
            cursor: pointer;
            transition: all 0.2s;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            gap: 0.35rem;
        }
        .btn-power-on { background: var(--accent-emerald); color: #022c22; }
        .btn-power-on:hover { filter: brightness(1.15); }
        .btn-power-off { background: var(--accent-rose); color: white; }
        .btn-power-off:hover { filter: brightness(1.15); }
        .btn-reboot { background: var(--accent-amber); color: #451a03; }
        .btn-reboot:hover { filter: brightness(1.15); }
        .btn-usb-boot { background: var(--accent-indigo); color: #ffffff; }
        .btn-usb-boot:hover { filter: brightness(1.15); }
        .btn-flash { background: linear-gradient(135deg, #06b6d4, #6366f1); color: #ffffff; grid-column: span 2; }
        .btn-flash:hover { filter: brightness(1.15); box-shadow: 0 0 12px rgba(6, 182, 212, 0.4); }
        .flash-form-group { display: flex; flex-direction: column; gap: 0.35rem; }
        .flash-form-label { font-size: 0.75rem; color: var(--text-muted); font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; }
        .flash-form-input {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 6px;
            color: var(--text-main);
            padding: 0.45rem 0.65rem;
            font-size: 0.85rem;
            font-family: monospace;
            outline: none;
            transition: border-color 0.2s;
        }
        .flash-form-input:focus {
            border-color: var(--accent-cyan);
            box-shadow: 0 0 8px rgba(6,182,212,0.25);
        }
        .flash-log-box {
            background: #050811;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 0.6rem;
            font-family: 'Lucida Console', 'Courier New', Monaco, monospace;
            font-size: 0.75rem;
            color: #94a3b8;
            max-height: 160px;
            overflow-y: auto;
            white-space: pre-wrap;
            word-break: break-all;
        }
        .btn-secondary { background: #22304d; color: var(--text-main); border: 1px solid var(--border); }
        .btn-secondary:hover { background: #2d3e63; }
        .btn-active { background: var(--accent-cyan); color: #050811; }
        .meta-row {
            display: flex;
            justify-content: space-between;
            font-size: 0.8rem;
            padding: 0.2rem 0;
            border-bottom: 1px dashed rgba(255,255,255,0.06);
        }
        .meta-label { color: var(--text-muted); }
        .meta-val { font-family: monospace; font-weight: 600; }
        .telemetry-summary {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 0.5rem;
        }
        .stat-box {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 0.4rem 0.5rem;
            text-align: center;
        }
        .stat-label { font-size: 0.7rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.05em; }
        .stat-value { font-size: 1.25rem; font-weight: 700; color: var(--accent-cyan); margin-top: 0.15rem; }
        .sparkline-container {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 0.35rem 0.5rem;
            display: flex;
            flex-direction: column;
            gap: 0.2rem;
        }
        .sparkline-title {
            display: flex;
            justify-content: space-between;
            font-size: 0.7rem;
            color: var(--text-muted);
        }
        .sparkline-svg {
            width: 100%;
            height: 36px;
            overflow: visible;
        }
        .voltages-grid {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 0.3rem;
            max-height: 180px;
            overflow-y: auto;
            padding-right: 0.25rem;
        }
        .voltage-item {
            background: #0d1424;
            border: 1px solid var(--border);
            padding: 0.25rem 0.4rem;
            border-radius: 4px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 0.75rem;
        }
        .voltage-name { color: var(--text-muted); font-size: 0.7rem; }
        .voltage-num { font-family: monospace; font-weight: 600; color: #38bdf8; }
        .cmd-input {
            background: var(--bg-term);
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 0.4rem 0.6rem;
            color: var(--text-main);
            font-family: monospace;
            font-size: 0.8rem;
            outline: none;
        }
        .cmd-input:focus { border-color: var(--accent-cyan); }
        /* Direct Interactive Terminal Styles */
        .terminal-container {
            display: flex;
            flex-direction: column;
            background: var(--bg-term);
            border: 1px solid var(--border);
            border-radius: 8px;
            overflow: hidden;
            flex: 1;
            min-height: 0;
            transition: all 0.2s ease;
        }
        .terminal-topbar {
            background: #0f1629;
            border-bottom: 1px solid var(--border);
            padding: 0.35rem 0.75rem;
            display: flex;
            align-items: center;
            justify-content: space-between;
            font-size: 0.75rem;
            color: var(--text-muted);
            gap: 0.5rem;
            flex-wrap: wrap;
        }
        .focus-badge {
            display: inline-flex;
            align-items: center;
            gap: 0.35rem;
            padding: 0.2rem 0.6rem;
            border-radius: 4px;
            font-size: 0.75rem;
            font-weight: 600;
            background: #1f293d;
            color: var(--text-muted);
            border: 1px solid var(--border);
            cursor: pointer;
            transition: all 0.2s;
        }
        .focus-badge.active {
            background: rgba(6,182,212,0.15);
            color: var(--accent-cyan);
            border-color: rgba(6,182,212,0.4);
            box-shadow: 0 0 8px rgba(6,182,212,0.25);
        }
        .terminal-screen {
            flex: 1;
            min-height: 0;
            padding: 0.35rem 0.5rem;
            background: var(--bg-term);
            outline: none;
            overflow: hidden;
            position: relative;
            border: 2px solid transparent;
            font-family: 'Lucida Console', 'Courier New', Monaco, monospace;
            transition: border-color 0.2s, box-shadow 0.2s;
        }
        .terminal-screen:focus, .terminal-screen:focus-within {
            border-color: rgba(6,182,212,0.6);
            box-shadow: inset 0 0 16px rgba(6,182,212,0.12);
        }
        .terminal-screen .xterm {
            height: 100%;
            padding: 0;
        }
        .terminal-screen .xterm-viewport {
            overflow-y: auto !important;
            scrollbar-width: thin;
            scrollbar-color: #1e293b #050811;
        }
        .terminal-screen .xterm-viewport::-webkit-scrollbar {
            width: 8px;
        }
        .terminal-screen .xterm-viewport::-webkit-scrollbar-track {
            background: #050811;
        }
        .terminal-screen .xterm-viewport::-webkit-scrollbar-thumb {
            background: #1e293b;
            border-radius: 4px;
        }
        .terminal-screen .xterm-viewport::-webkit-scrollbar-thumb:hover {
            background: #334155;
        }
        .terminal-cursor {
            display: inline-block;
            width: 8px;
            height: 1.15em;
            background: var(--accent-cyan);
            vertical-align: text-bottom;
            margin-left: 1px;
            animation: blink 1s step-end infinite;
        }
        .terminal-screen:not(:focus) .terminal-cursor {
            background: transparent;
            border: 1px solid var(--text-muted);
            animation: none;
        }
        @keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0; } }

        /* Safety Modal */
        .modal-backdrop {
            position: fixed;
            top: 0;
            left: 0;
            width: 100vw;
            height: 100vh;
            background: rgba(0,0,0,0.7);
            backdrop-filter: blur(4px);
            z-index: 20000;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 1rem;
        }
        .modal-card {
            background: var(--bg-card);
            border: 1px solid var(--border);
            border-radius: 12px;
            max-width: 440px;
            width: 100%;
            padding: 1.5rem;
            display: flex;
            flex-direction: column;
            gap: 1.25rem;
            box-shadow: 0 10px 30px rgba(0,0,0,0.5);
        }
        .modal-title {
            font-size: 1.15rem;
            font-weight: 700;
            display: flex;
            align-items: center;
            gap: 0.5rem;
        }
        .modal-desc {
            font-size: 0.9rem;
            color: var(--text-muted);
            line-height: 1.5;
        }
        .modal-actions {
            display: flex;
            justify-content: flex-end;
            gap: 0.75rem;
        }

        /* Responsive Media Queries */
        @media (max-width: 1024px) {
            .dashboard-grid {
                grid-template-columns: 1fr;
            }
            main {
                padding: 1rem;
            }
            .terminal-container {
                height: 540px;
            }
        }
        @media (max-width: 640px) {
            header {
                flex-direction: column;
                align-items: flex-start;
                gap: 0.75rem;
                padding: 0.75rem 1rem;
            }
            .header-meta {
                width: 100%;
                justify-content: space-between;
            }
            .card-header {
                flex-direction: column;
                align-items: flex-start;
            }
            .btn-group {
                flex-wrap: wrap;
                width: 100%;
            }
            .btn-group .btn {
                flex: 1;
            }
        }
    </style>
</head>
<body>
    <header>
        <div class="brand">
            <div class="brand-logo">&#x2699;</div>
            <div>
                <span class="brand-title">embdevenv</span>
                <span class="brand-sub">Gateway Daemon</span>
            </div>
        </div>
        <div class="header-meta">
            <div id="connectionPill" class="status-pill">
                <span class="status-dot"></span>
                <span id="connectionText">Daemon Connected</span>
            </div>
            <div id="clockDisplay">--:--:--</div>
        </div>
    </header>

    <main>
        <div id="targetTabs" class="tabs-container">
            <!-- Dynamically populated target tabs -->
        </div>

        <div class="dashboard-grid">
            <!-- Left Column: Controls & Telemetry -->
            <div style="display:flex; flex-direction:column; gap:0.75rem; height:495px; box-sizing:border-box;">
                <div class="card">
                    <div class="card-header">
                        <span class="card-title">&#x26A1; Power & Target State</span>
                        <span id="powerBadge" class="status-pill" style="background:rgba(244,63,94,0.15); color:var(--accent-rose); border-color:rgba(244,63,94,0.3)">OFF</span>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-power-on" onclick="controlPower('on')">&#x25B6; Power ON</button>
                        <button class="btn btn-power-off" onclick="requestPowerOff()">&#x25A0; Power OFF</button>
                        <button class="btn btn-reboot" onclick="requestReboot()">&#x21BB; Reboot</button>
                        <button class="btn btn-usb-boot" onclick="requestUsbBoot()">&#x1F50C; USB Boot</button>
                        <button class="btn btn-flash" onclick="openFlashModal()">&#x26A1; Flash Firmware</button>
                    </div>
                    <div>
                        <div class="meta-row">
                            <span class="meta-label">Board Model</span>
                            <span id="targetModel" class="meta-val">-</span>
                        </div>
                        <div class="meta-row">
                            <span class="meta-label">Consoles</span>
                            <span id="targetConsolesCount" class="meta-val">-</span>
                        </div>
                        <div class="meta-row">
                            <span class="meta-label">MCU Port</span>
                            <span id="targetMcu" class="meta-val">-</span>
                        </div>
                    </div>
                </div>

                <div class="card" style="flex:1; display:flex; flex-direction:column; justify-content:space-between;">
                    <div class="card-header">
                        <span class="card-title">&#x1F4CA; Live Telemetry</span>
                        <span id="telemetryTime" style="font-size:0.75rem; color:var(--text-muted)">-</span>
                    </div>
                    <div class="telemetry-summary">
                        <div class="stat-box">
                            <div class="stat-label">Core Temp</div>
                            <div id="statTemp" class="stat-value">-- &deg;C</div>
                        </div>
                        <div class="stat-box">
                            <div class="stat-label">Fan Speed</div>
                            <div id="statFan" class="stat-value">-- RPM</div>
                        </div>
                    </div>
                    <div class="sparkline-container">
                        <div class="sparkline-title">
                            <span>Temp Trend (30s)</span>
                            <span id="sparklineStats">-- &deg;C</span>
                        </div>
                        <svg id="tempSparkline" class="sparkline-svg" viewBox="0 0 280 48" preserveAspectRatio="none">
                            <path id="sparklinePath" fill="rgba(6,182,212,0.15)" stroke="#06b6d4" stroke-width="2" d=""></path>
                        </svg>
                    </div>
                    <div>
                        <div style="font-size:0.8rem; font-weight:600; color:var(--text-muted); margin-bottom:0.5rem;">Voltage Rails</div>
                        <div id="voltagesContainer" class="voltages-grid">
                            <span style="color:var(--text-muted); font-size:0.8rem;">Waiting for telemetry...</span>
                        </div>
                    </div>
                </div>

            </div>

            <!-- Right Column: Live Direct-Interactive Serial Console -->
            <div class="console-card">
                <div class="card-header" style="flex-wrap:wrap; gap:0.75rem;">
                    <div style="display:flex; align-items:center; gap:0.6rem; flex-wrap:wrap;">
                        <span class="card-title">&#x1F4BB; Serial Console</span>
                        <select id="consoleSelect" class="cmd-input" style="padding:0.25rem 0.5rem; font-size:0.8rem; width:auto; border-radius:4px; background:#0d1424; color:var(--text-main); cursor:pointer;" onchange="onConsoleChanged(this.value)">
                            <option value="">(Loading consoles...)</option>
                        </select>
                    </div>
                    <div style="display:flex; align-items:center; gap:0.5rem; flex-wrap:wrap;">
                        <span id="telnetHint" style="font-family:monospace; font-size:0.8rem; color:var(--accent-cyan); background:#0d1424; padding:0.2rem 0.5rem; border-radius:4px;">telnet -- 6060</span>
                        <button id="freezeBtn" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.75rem;" onclick="toggleFreeze()" title="Pause console output rendering">&#x23F8; Freeze</button>
                        <button class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.75rem;" onclick="exportConsoleLog()" title="Export console buffer to text file">&#x1F4BE; Export</button>
                        <button id="expandBtn" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.75rem;" onclick="toggleFullscreen()" title="Toggle Fullscreen Terminal">&#x26F6; Expand</button>
                        <button class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.75rem;" onclick="clearConsole()">Clear</button>
                    </div>
                </div>
                <div id="terminalContainer" class="terminal-container">
                    <div class="terminal-topbar">
                        <div style="display:flex; align-items:center; gap:0.6rem;">
                            <span id="terminalFocusBadge" class="focus-badge" onclick="focusTerminal()">Click terminal to interact</span>
                            <span id="consoleStats">Connecting...</span>
                        </div>
                        <div style="display:flex; align-items:center; gap:0.6rem;">
                            <label style="display:flex; align-items:center; gap:0.4rem; cursor:pointer;">
                                <input id="autoScrollCheck" type="checkbox" checked> Auto-scroll
                            </label>
                            <button id="topbarExpandBtn" class="btn btn-secondary" style="padding:0.15rem 0.5rem; font-size:0.75rem;" onclick="toggleFullscreen()" title="Toggle Fullscreen Terminal">&#x26F6; Expand</button>
                        </div>
                    </div>
                    <!-- Direct keyboard interactive terminal screen: No detached input box! -->
                    <div id="terminalScreen" class="terminal-screen" tabindex="0" title="Click to focus terminal. Keystrokes, Tab, Backspace, Ctrl-C, arrows are sent directly to UART.">
                        <span id="terminalContent"></span><span id="terminalCursor" class="terminal-cursor"></span>
                    </div>
                </div>
            </div>
        </div>
    </main>

    <!-- Safety Confirmation Modal -->
    <div id="safetyModal" class="modal-backdrop" style="display:none;">
        <div class="modal-card">
            <div id="modalTitle" class="modal-title">&#x26A0;&#xFE0F; Confirm Action</div>
            <p id="modalDesc" class="modal-desc"></p>
            <div class="modal-actions">
                <button class="btn btn-secondary" onclick="closeSafetyModal()">Cancel</button>
                <button id="modalConfirmBtn" class="btn btn-power-off" onclick="executePendingPowerAction()">Proceed</button>
            </div>
        </div>
    </div>

    <!-- Flash Firmware Modal -->
    <div id="flashModal" class="modal-backdrop" style="display:none;">
        <div class="modal-card" style="max-width:560px;">
            <div class="modal-title" style="color:var(--accent-cyan);">
                <span>&#x26A1;</span> Flash Firmware: <span id="flashTargetName" style="color:var(--text-main);">-</span>
            </div>
            <div class="modal-desc" style="display:flex; flex-direction:column; gap:0.8rem;">
                <div style="display:grid; grid-template-columns: 1fr 1fr; gap:0.6rem;">
                    <div class="flash-form-group">
                        <label class="flash-form-label">Chip Architecture</label>
                        <input id="flashChipInput" class="flash-form-input" type="text" placeholder="cv3ad655">
                    </div>
                    <div class="flash-form-group">
                        <label class="flash-form-label">Force USB Boot Mode</label>
                        <label style="display:flex; align-items:center; gap:0.5rem; height:34px; cursor:pointer; font-size:0.85rem; color:var(--text-main);">
                            <input id="flashForceUsbBoot" type="checkbox" checked>
                            <span>MCU reset -usb -y</span>
                        </label>
                    </div>
                </div>

                <div class="flash-form-group">
                    <label class="flash-form-label">Firmware Package (*.bin / *.pkg)</label>
                    <input id="flashFwInput" class="flash-form-input" type="text" placeholder="/path/to/firmware.bin">
                </div>

                <div style="display:grid; grid-template-columns: 1fr 1fr; gap:0.6rem;">
                    <div class="flash-form-group">
                        <label class="flash-form-label">UST Image (Optional)</label>
                        <input id="flashUstInput" class="flash-form-input" type="text" placeholder="/path/to/ust.bin">
                    </div>
                    <div class="flash-form-group">
                        <label class="flash-form-label">BLD Image (Optional)</label>
                        <input id="flashBldInput" class="flash-form-input" type="text" placeholder="/path/to/bld.bin">
                    </div>
                </div>

                <div style="background:#0d1424; border:1px solid var(--border); border-radius:6px; padding:0.5rem 0.75rem; display:flex; align-items:center; justify-content:space-between;">
                    <div style="display:flex; align-items:center; gap:0.5rem; font-size:0.8rem;">
                        <span style="color:var(--text-muted);">USB Device:</span>
                        <span id="flashUsbStatus" class="status-pill" style="font-size:0.7rem;">Checking...</span>
                    </div>
                    <button class="btn btn-secondary" style="padding:0.25rem 0.55rem; font-size:0.75rem;" onclick="scanUsbDevices()">&#x1F50D; Scan USB</button>
                </div>

                <div id="flashStatusArea" style="display:none; flex-direction:column; gap:0.4rem;">
                    <div style="display:flex; justify-content:space-between; align-items:center; font-size:0.8rem;">
                        <span id="flashStatusMsg" style="font-weight:600; color:var(--accent-cyan);">Flashing in progress...</span>
                        <span id="flashTimer" style="color:var(--text-muted); font-size:0.75rem;"></span>
                    </div>
                    <pre id="flashLogArea" class="flash-log-box"></pre>
                </div>
            </div>
            <div class="modal-actions">
                <button id="flashCancelBtn" class="btn btn-secondary" onclick="closeFlashModal()">Cancel</button>
                <button id="flashSubmitBtn" class="btn btn-flash" style="grid-column:unset;" onclick="executeFlashFirmware()">&#x26A1; Flash Now</button>
            </div>
        </div>
    </div>

    <script>
        let targets = [];
        let activeTarget = '';
        let activeConsole = '';
        let ws = null;
        let wsReconnectTimer = null;
        let isFrozen = false;
        let frozenBuffer = [];
        let rawLogLines = [];
        let pendingPowerAction = null;

        function updateClock() {
            const now = new Date();
            document.getElementById('clockDisplay').textContent = now.toTimeString().split(' ')[0];
        }
        setInterval(updateClock, 1000);
        updateClock();

        // Safety Modal Handlers
        function requestPowerOff() {
            pendingPowerAction = 'off';
            document.getElementById('modalTitle').innerHTML = '&#x26A0;&#xFE0F; Power OFF Target';
            document.getElementById('modalDesc').textContent = `Are you sure you want to cut power to target "${activeTarget}"? Any unsaved runtime data or active processes will be terminated immediately.`;
            const btn = document.getElementById('modalConfirmBtn');
            btn.className = 'btn btn-power-off';
            btn.textContent = 'Cut Power';
            document.getElementById('safetyModal').style.display = 'flex';
        }

        function requestReboot() {
            pendingPowerAction = 'reboot';
            document.getElementById('modalTitle').innerHTML = '&#x21BB; Reboot Target';
            document.getElementById('modalDesc').textContent = `Are you sure you want to reboot target "${activeTarget}"? The board will be power cycled.`;
            const btn = document.getElementById('modalConfirmBtn');
            btn.className = 'btn btn-reboot';
            btn.textContent = 'Reboot Target';
            document.getElementById('safetyModal').style.display = 'flex';
        }

        function requestUsbBoot() {
            pendingPowerAction = 'usb_download';
            document.getElementById('modalTitle').innerHTML = '&#x1F50C; Enter USB Download Mode';
            document.getElementById('modalDesc').textContent = `Are you sure you want to put target "${activeTarget}" into USB download mode for flashing? The SoC will reboot into USB bootloader mode.`;
            const btn = document.getElementById('modalConfirmBtn');
            btn.className = 'btn btn-usb-boot';
            btn.textContent = 'Enter USB Boot';
            document.getElementById('safetyModal').style.display = 'flex';
        }

        function closeSafetyModal() {
            document.getElementById('safetyModal').style.display = 'none';
            pendingPowerAction = null;
        }

        // Flash Modal Handlers
        let isFlashingOperation = false;

        async function openFlashModal() {
            if (!activeTarget) return;
            const targetObj = targets.find(t => t.name === activeTarget);
            document.getElementById('flashTargetName').textContent = activeTarget;
            
            const chipInput = document.getElementById('flashChipInput');
            const fwInput = document.getElementById('flashFwInput');
            const ustInput = document.getElementById('flashUstInput');
            const bldInput = document.getElementById('flashBldInput');
            const forceUsb = document.getElementById('flashForceUsbBoot');

            const fl = (targetObj && targetObj.flashing) ? targetObj.flashing : {};
            chipInput.value = fl.chip || 'cv3ad655';
            fwInput.value = fl.defaultFwImage || '';
            ustInput.value = fl.ustImage || '';
            bldInput.value = fl.bldImage || '';
            forceUsb.checked = true;

            document.getElementById('flashStatusArea').style.display = 'none';
            document.getElementById('flashLogArea').textContent = '';
            document.getElementById('flashSubmitBtn').disabled = false;
            document.getElementById('flashCancelBtn').disabled = false;
            document.getElementById('flashModal').style.display = 'flex';

            await scanUsbDevices();
        }

        function closeFlashModal() {
            if (isFlashingOperation) {
                if (!confirm("Flashing is currently in progress. Close modal anyway?")) return;
            }
            document.getElementById('flashModal').style.display = 'none';
        }

        async function scanUsbDevices() {
            const statusEl = document.getElementById('flashUsbStatus');
            statusEl.textContent = 'Scanning...';
            statusEl.style.background = 'rgba(255,255,255,0.06)';
            statusEl.style.color = 'var(--text-muted)';
            statusEl.style.borderColor = 'var(--border)';

            try {
                const res = await fetch('/api/usb/devices');
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                if (data.recovery_device_detected) {
                    statusEl.textContent = 'Recovery Mode (4255:001d)';
                    statusEl.style.background = 'rgba(16,185,129,0.15)';
                    statusEl.style.color = 'var(--accent-emerald)';
                    statusEl.style.borderColor = 'rgba(16,185,129,0.3)';
                } else if (data.devices && data.devices.length > 0) {
                    statusEl.textContent = `${data.devices.length} USB dev(s)`;
                    statusEl.style.background = 'rgba(6,182,212,0.15)';
                    statusEl.style.color = 'var(--accent-cyan)';
                    statusEl.style.borderColor = 'rgba(6,182,212,0.3)';
                } else {
                    statusEl.textContent = 'No devices found';
                    statusEl.style.background = 'rgba(244,63,94,0.15)';
                    statusEl.style.color = 'var(--accent-rose)';
                    statusEl.style.borderColor = 'rgba(244,63,94,0.3)';
                }
            } catch (err) {
                statusEl.textContent = 'Scan error';
                statusEl.style.background = 'rgba(244,63,94,0.15)';
                statusEl.style.color = 'var(--accent-rose)';
            }
        }

        async function executeFlashFirmware() {
            if (!activeTarget || isFlashingOperation) return;

            const chip = document.getElementById('flashChipInput').value.trim();
            const fwImage = document.getElementById('flashFwInput').value.trim();
            const ustImage = document.getElementById('flashUstInput').value.trim();
            const bldImage = document.getElementById('flashBldInput').value.trim();
            const forceUsbBoot = document.getElementById('flashForceUsbBoot').checked;

            if (!fwImage) {
                alert('Please specify the Firmware Package image path.');
                return;
            }

            isFlashingOperation = true;
            const submitBtn = document.getElementById('flashSubmitBtn');
            const cancelBtn = document.getElementById('flashCancelBtn');
            const statusArea = document.getElementById('flashStatusArea');
            const statusMsg = document.getElementById('flashStatusMsg');
            const logArea = document.getElementById('flashLogArea');

            submitBtn.disabled = true;
            submitBtn.textContent = 'Flashing...';
            cancelBtn.disabled = true;
            statusArea.style.display = 'flex';
            statusMsg.textContent = forceUsbBoot ? 'Rebooting target into USB download mode...' : 'Invoking usb-matrix...';
            statusMsg.style.color = 'var(--accent-cyan)';
            logArea.textContent = 'Initiating firmware flash sequence for ' + activeTarget + '...\n';

            const startTime = Date.now();
            const timerEl = document.getElementById('flashTimer');
            const timerInterval = setInterval(() => {
                const elapsedSec = ((Date.now() - startTime) / 1000).toFixed(1);
                timerEl.textContent = `${elapsedSec}s`;
            }, 200);

            try {
                const res = await fetch(`/api/targets/${activeTarget}/flash`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        chip: chip || undefined,
                        fw_image: fwImage,
                        ust_image: ustImage || undefined,
                        bld_image: bldImage || undefined,
                        force_usb_boot: forceUsbBoot
                    })
                });

                clearInterval(timerInterval);
                const data = await res.json();

                if (data.success) {
                    statusMsg.textContent = `✓ Firmware Flashing Succeeded! (${(data.duration_ms/1000).toFixed(1)}s)`;
                    statusMsg.style.color = 'var(--accent-emerald)';
                } else {
                    statusMsg.textContent = `✗ Flashing Failed (${data.stage || 'error'}): ${data.error || 'Unknown error'}`;
                    statusMsg.style.color = 'var(--accent-rose)';
                }

                logArea.textContent = data.usb_matrix_log || data.error || JSON.stringify(data, null, 2);
                logArea.scrollTop = logArea.scrollHeight;
            } catch (err) {
                clearInterval(timerInterval);
                statusMsg.textContent = '✗ Request failed: ' + err.message;
                statusMsg.style.color = 'var(--accent-rose)';
                logArea.textContent = String(err);
            } finally {
                isFlashingOperation = false;
                submitBtn.disabled = false;
                submitBtn.textContent = '⚡ Flash Now';
                cancelBtn.disabled = false;
                setTimeout(pollActiveTarget, 1000);
            }
        }

        async function executePendingPowerAction() {
            if (!pendingPowerAction) return;
            const action = pendingPowerAction;
            closeSafetyModal();
            await controlPower(action);
        }

        // Fullscreen Terminal Toggle
        function toggleFullscreen() {
            const card = document.querySelector('.console-card');
            const btn = document.getElementById('expandBtn');
            const topBtn = document.getElementById('topbarExpandBtn');
            if (!card) return;
            card.classList.toggle('fullscreen');
            const isFull = card.classList.contains('fullscreen');
            const label = isFull ? '&#x1F5D7; Compress' : '&#x26F6; Expand';
            if (btn) {
                btn.innerHTML = label;
                if (isFull) btn.classList.add('btn-active'); else btn.classList.remove('btn-active');
            }
            if (topBtn) {
                topBtn.innerHTML = label;
                if (isFull) topBtn.classList.add('btn-active'); else topBtn.classList.remove('btn-active');
            }
            if (isFull) {
                card.scrollTop = 0;
                if (fitAddon) {
                    setTimeout(() => fitAddon.fit(), 50);
                }
            } else {
                if (term) {
                    term.resize(80, 24);
                }
            }
            focusTerminal();
        }

        window.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                const card = document.querySelector('.console-card');
                if (card && card.classList.contains('fullscreen')) {
                    toggleFullscreen();
                }
            }
        }, true);

        let terminalLines = [''];
        let renderScheduled = false;

        // Freeze Toggle
        function toggleFreeze() {
            isFrozen = !isFrozen;
            const btn = document.getElementById('freezeBtn');
            if (isFrozen) {
                btn.innerHTML = '&#x25B6; Resume';
                btn.classList.add('btn-active');
            } else {
                btn.innerHTML = '&#x23F8; Freeze';
                btn.classList.remove('btn-active');
                if (frozenBuffer.length > 0) {
                    if (term) {
                        for (const chunk of frozenBuffer) {
                            term.write(chunk);
                        }
                    } else {
                        for (const chunk of frozenBuffer) {
                            applyTerminalStream(chunk);
                        }
                        scheduleTerminalRender();
                    }
                    frozenBuffer = [];
                }
            }
        }

        // Export Log
        function exportConsoleLog() {
            let content = '';
            if (term && term.buffer && term.buffer.active) {
                const buffer = term.buffer.active;
                for (let i = 0; i < buffer.length; i++) {
                    const line = buffer.getLine(i);
                    if (line) content += line.translateToString(true) + '\n';
                }
            } else {
                content = terminalLines.join('\n');
            }
            const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `console_${activeTarget}_${activeConsole || 'default'}_${new Date().toISOString().replace(/[:.]/g, '-')}.log`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        }

        let pendingEscape = '';

        // ANSI Color & SGR parser
        function escapeHtml(str) {
            if (typeof str !== 'string') {
                str = (str === null || str === undefined) ? '' : String(str);
            }
            return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        }

        function parseAnsiToHtml(text) {
            if (typeof text !== 'string') text = String(text || '');
            const escaped = escapeHtml(text);
            const withColors = escaped.replace(/\x1b\[([0-9;]*)m/g, (match, p1) => {
                if (!p1 || p1 === '0') return '</span>';
                const codes = p1.split(';').map(Number);
                let styles = [];
                for (let code of codes) {
                    if (code === 0) return '</span>';
                    if (code === 1) styles.push('font-weight:bold');
                    if (code === 4) styles.push('text-decoration:underline');
                    if (code >= 30 && code <= 37) {
                        const colors = ['#000', '#f43f5e', '#10b981', '#f59e0b', '#38bdf8', '#c084fc', '#06b6d4', '#f1f5f9'];
                        styles.push(`color:${colors[code - 30]}`);
                    } else if (code >= 90 && code <= 97) {
                        const bright = ['#64748b', '#fb7185', '#34d399', '#fbbf24', '#60a5fa', '#e879f9', '#22d3ee', '#ffffff'];
                        styles.push(`color:${bright[code - 90]}`);
                    }
                }
                return styles.length > 0 ? `<span style="${styles.join(';')}">` : '';
            });

            // Strip all remaining control characters and escape sequences
            return withColors
                .replace(/\x1b\][^\x07\x1b]*(\x07|\x1b\\)/g, '')
                .replace(/\x1b\[[0-9;?>=<]*[a-zA-Z~]/g, '')
                .replace(/\x1b[()#%*+][A-Za-z0-9]/g, '')
                .replace(/\x1b[=><78DEMN]/g, '')
                .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, '');
        }

        function applyTerminalStream(chunk) {
            if (typeof chunk !== 'string') chunk = String(chunk || '');

            // Prepend incomplete escape sequence from previous chunk
            chunk = pendingEscape + chunk;
            pendingEscape = '';

            // If chunk ends with an incomplete escape sequence or trailing \r, hold it back
            const incompleteMatch = chunk.match(/(\x1b(\[[0-9;?>=<]*|\][^\x07\x1b]*|[()#%*+][A-Za-z0-9]?|)|\r+)$/);
            if (incompleteMatch) {
                pendingEscape = incompleteMatch[0];
                chunk = chunk.slice(0, chunk.length - pendingEscape.length);
            }
            if (!chunk) return;

            // Strip non-visual escape sequences and control characters
            chunk = chunk.replace(/\x1b\][^\x07\x1b]*(\x07|\x1b\\)/g, '')
                         .replace(/\x1b[()#%*+][A-Za-z0-9]/g, '')
                         .replace(/\x1b[=><78DEMN]/g, '')
                         .replace(/\x1b\[\?[0-9;]*[a-zA-Z]/g, '')
                         .replace(/\x1b\[[0-9;]*[nc]/g, '')
                         .replace(/\x1b\[>[0-9;]*c/g, '')
                         .replace(/[\x00\x07\x0b\x0e\x0f\x7f]/g, '');

            // Normalize \r+\n to single \n
            chunk = chunk.replace(/\r+\n/g, '\n');

            for (let i = 0; i < chunk.length; i++) {
                const c = chunk[i];
                if (c === '\n') {
                    terminalLines.push('');
                } else if (c === '\r') {
                    // Lone \r (not followed by \n): carriage return resets line
                    terminalLines[terminalLines.length - 1] = '';
                } else if (c === '\b' || c === '\x08') {
                    let last = terminalLines[terminalLines.length - 1];
                    if (last.length > 0) {
                        terminalLines[terminalLines.length - 1] = last.slice(0, -1);
                    }
                } else if (c === '\x1b' && i + 1 < chunk.length && chunk[i + 1] === '[') {
                    let match = chunk.slice(i).match(/^\x1b\[([0-9;?>=<]*)([a-zA-Z~])/);
                    if (match) {
                        const param = match[1];
                        const cmd = match[2];
                        if (cmd === 'K') {
                            if (param === '2' || param === '' || param === '0') {
                                terminalLines[terminalLines.length - 1] = '';
                            }
                        } else if (cmd === 'J') {
                            if (param === '2' || param === '3') {
                                terminalLines = [''];
                            }
                        } else if (cmd === 'm') {
                            terminalLines[terminalLines.length - 1] += match[0];
                        }
                        i += match[0].length - 1;
                        continue;
                    } else {
                        // Skip unknown escape so it never leaks
                        let skipMatch = chunk.slice(i).match(/^\x1b\[[^a-zA-Z~]*[a-zA-Z~]/);
                        if (skipMatch) {
                            i += skipMatch[0].length - 1;
                            continue;
                        }
                    }
                } else {
                    terminalLines[terminalLines.length - 1] += c;
                }
            }

            if (terminalLines.length > 1500) {
                terminalLines.splice(0, terminalLines.length - 1500);
            }
        }

        function scheduleTerminalRender() {
            if (renderScheduled) return;
            renderScheduled = true;
            requestAnimationFrame(() => {
                renderScheduled = false;
                renderTerminal();
            });
        }

        function renderTerminal() {
            const raw = terminalLines.join('\n');
            const html = parseAnsiToHtml(raw);
            const contentSpan = document.getElementById('terminalContent');
            contentSpan.innerHTML = html;

            if (document.getElementById('autoScrollCheck').checked) {
                const screen = document.getElementById('terminalScreen');
                screen.scrollTop = screen.scrollHeight;
            }
        }

        async function processIncomingTerminalData(data) {
            let text = '';
            if (typeof data === 'string') {
                text = data;
            } else if (data instanceof ArrayBuffer) {
                text = new TextDecoder().decode(data);
            } else if (data instanceof Blob) {
                text = await data.text();
            } else {
                text = String(data || '');
            }

            if (isFrozen) {
                frozenBuffer.push(text);
            } else {
                if (term) {
                    term.write(text);
                    if (document.getElementById('autoScrollCheck').checked) {
                        term.scrollToBottom();
                    }
                } else {
                    applyTerminalStream(text);
                    scheduleTerminalRender();
                }
            }
        }

        let term = null;
        let fitAddon = null;

        function focusTerminal() {
            if (term) {
                term.focus();
            } else {
                const screen = document.getElementById('terminalScreen');
                if (screen) screen.focus();
            }
        }

        function initTerminal() {
            const screen = document.getElementById('terminalScreen');
            const badge = document.getElementById('terminalFocusBadge');

            if (typeof Terminal !== 'undefined') {
                screen.innerHTML = '';
                term = new Terminal({
                    cols: 80,
                    rows: 24,
                    cursorBlink: true,
                    cursorStyle: 'block',
                    fontSize: 14,
                    lineHeight: 1.15,
                    fontFamily: "'Lucida Console', 'Courier New', Monaco, monospace",
                    theme: {
                        background: '#050811',
                        foreground: '#e2e8f0',
                        cursor: '#06b6d4',
                        cursorAccent: '#050811',
                        selectionBackground: 'rgba(6, 182, 212, 0.3)',
                        black: '#0f172a',
                        red: '#f43f5e',
                        green: '#10b981',
                        yellow: '#f59e0b',
                        blue: '#38bdf8',
                        magenta: '#c084fc',
                        cyan: '#06b6d4',
                        white: '#f1f5f9',
                        brightBlack: '#64748b',
                        brightRed: '#fb7185',
                        brightGreen: '#34d399',
                        brightYellow: '#fbbf24',
                        brightBlue: '#60a5fa',
                        brightMagenta: '#e879f9',
                        brightCyan: '#22d3ee',
                        brightWhite: '#ffffff'
                    },
                    scrollback: 5000,
                    convertEol: true
                });

                if (typeof FitAddon !== 'undefined' && FitAddon.FitAddon) {
                    fitAddon = new FitAddon.FitAddon();
                    term.loadAddon(fitAddon);
                }

                term.open(screen);
                term.resize(80, 24);

                term.attachCustomKeyEventHandler((e) => {
                    if (e.key === 'Escape' && e.type === 'keydown') {
                        const card = document.querySelector('.console-card');
                        if (card && card.classList.contains('fullscreen')) {
                            toggleFullscreen();
                            return false;
                        }
                    }
                    return true;
                });

                let resizeTimer = null;
                window.addEventListener('resize', () => {
                    if (resizeTimer) clearTimeout(resizeTimer);
                    resizeTimer = setTimeout(() => {
                        const card = document.querySelector('.console-card');
                        if (card && card.classList.contains('fullscreen')) {
                            if (fitAddon) {
                                try { fitAddon.fit(); } catch(e) {}
                            }
                        } else {
                            if (term) {
                                term.resize(80, 24);
                            }
                        }
                    }, 80);
                });

                screen.addEventListener('focusin', () => {
                    badge.classList.add('active');
                    badge.innerHTML = '&#x25CF; Live Terminal (Active)';
                });

                screen.addEventListener('focusout', () => {
                    badge.classList.remove('active');
                    badge.innerHTML = 'Click terminal to interact';
                });

                term.onData((data) => {
                    if (ws && ws.readyState === WebSocket.OPEN) {
                        ws.send(data);
                    }
                });

                const container = document.getElementById('terminalContainer');
                if (container) {
                    container.addEventListener('click', () => {
                        if (term) term.focus();
                    });
                }
                screen.addEventListener('click', () => {
                    if (term) term.focus();
                });
                badge.addEventListener('click', () => {
                    if (term) term.focus();
                });
            } else {
                console.warn('xterm.js not loaded, using fallback DOM terminal');
                initTerminalKeyboardFallback();
            }
        }

        function initTerminalKeyboardFallback() {
            const screen = document.getElementById('terminalScreen');
            const badge = document.getElementById('terminalFocusBadge');

            screen.addEventListener('focus', () => {
                badge.classList.add('active');
                badge.innerHTML = '&#x25CF; Live Terminal (Active)';
            });

            screen.addEventListener('blur', () => {
                badge.classList.remove('active');
                badge.innerHTML = 'Click terminal to interact';
            });

            screen.addEventListener('keydown', (e) => {
                if (!ws || ws.readyState !== WebSocket.OPEN) return;

                // Handle Control Key combinations
                if (e.ctrlKey && !e.altKey && !e.metaKey) {
                    const key = e.key.toLowerCase();
                    if (key >= 'a' && key <= 'z') {
                        e.preventDefault();
                        const code = key.charCodeAt(0) - 96; // Ctrl+A=1, Ctrl+C=3, Ctrl+D=4
                        ws.send(String.fromCharCode(code));
                        return;
                    }
                }

                // Handle Special Keys
                if (e.key === 'Enter') {
                    e.preventDefault();
                    ws.send('\r');
                } else if (e.key === 'Backspace') {
                    e.preventDefault();
                    ws.send('\x08');
                } else if (e.key === 'Tab') {
                    e.preventDefault();
                    ws.send('\t');
                } else if (e.key === 'Escape') {
                    e.preventDefault();
                    ws.send('\x1b');
                } else if (e.key === 'ArrowUp') {
                    e.preventDefault();
                    ws.send('\x1b[A');
                } else if (e.key === 'ArrowDown') {
                    e.preventDefault();
                    ws.send('\x1b[B');
                } else if (e.key === 'ArrowRight') {
                    e.preventDefault();
                    ws.send('\x1b[C');
                } else if (e.key === 'ArrowLeft') {
                    e.preventDefault();
                    ws.send('\x1b[D');
                } else if (e.key === 'Home') {
                    e.preventDefault();
                    ws.send('\x1b[H');
                } else if (e.key === 'End') {
                    e.preventDefault();
                    ws.send('\x1b[F');
                } else if (e.key === 'Delete') {
                    e.preventDefault();
                    ws.send('\x1b[3~');
                } else if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
                    e.preventDefault();
                    ws.send(e.key);
                }
            });

            screen.addEventListener('paste', (e) => {
                e.preventDefault();
                const pasteData = (e.clipboardData || window.clipboardData).getData('text');
                if (pasteData && ws && ws.readyState === WebSocket.OPEN) {
                    ws.send(pasteData.replace(/\r?\n/g, '\r'));
                }
            });
        }

        // WebSocket Stream Connection
        function connectWebSocket() {
            if (ws) {
                try { ws.close(); } catch(e) {}
                ws = null;
            }
            if (wsReconnectTimer) {
                clearTimeout(wsReconnectTimer);
                wsReconnectTimer = null;
            }
            if (!activeTarget) return;

            const loc = window.location;
            const wsProto = loc.protocol === 'https:' ? 'wss:' : 'ws:';
            const conParam = activeConsole ? `?console=${encodeURIComponent(activeConsole)}` : '';
            const wsUrl = `${wsProto}//${loc.host}/ws/targets/${activeTarget}/console${conParam}`;

            const stats = document.getElementById('consoleStats');
            stats.textContent = 'Connecting WebSocket...';

            ws = new WebSocket(wsUrl);
            ws.binaryType = 'arraybuffer';

            ws.onopen = () => {
                stats.textContent = `Live WebSocket Connected (${activeConsole || 'default'})`;
                if (term) {
                    term.clear();
                    const card = document.querySelector('.console-card');
                    if (card && card.classList.contains('fullscreen')) {
                        if (fitAddon) fitAddon.fit();
                    } else {
                        term.resize(80, 24);
                    }
                } else {
                    terminalLines = [''];
                    scheduleTerminalRender();
                }
            };

            ws.onmessage = (event) => {
                processIncomingTerminalData(event.data);
            };

            ws.onclose = () => {
                stats.textContent = 'WebSocket Disconnected (reconnecting in 3s...)';
                wsReconnectTimer = setTimeout(connectWebSocket, 3000);
            };

            ws.onerror = (err) => {
                console.error('WebSocket error', err);
                stats.textContent = 'WebSocket Connection Error';
            };
        }

        async function fetchTargets() {
            try {
                const res = await fetch('/api/targets');
                if (!res.ok) return;
                const data = await res.json();
                targets = data.targets || [];
                renderTabs();
                if (targets.length > 0 && !activeTarget) {
                    selectTarget(targets[0].name);
                }
            } catch (err) {
                console.error('Failed to load targets', err);
            }
        }

        function renderTabs() {
            const tabs = document.getElementById('targetTabs');
            tabs.innerHTML = '';
            targets.forEach(t => {
                const btn = document.createElement('button');
                btn.className = 'tab-btn' + (t.name === activeTarget ? ' active' : '');
                btn.innerHTML = `<span>&#x1F4DF;</span> ${t.name}`;
                btn.onclick = () => selectTarget(t.name);
                tabs.appendChild(btn);
            });
        }

        function selectTarget(name) {
            activeTarget = name;
            activeConsole = '';
            renderTabs();
            pollActiveTarget().then(() => {
                connectWebSocket();
            });
        }

        function updateConsolesDropdown(consoles) {
            const select = document.getElementById('consoleSelect');
            if (!consoles || consoles.length === 0) {
                select.innerHTML = '<option value="">No console configured</option>';
                return;
            }
            const currentVal = activeConsole;
            let optionsHtml = '';
            let found = false;
            consoles.forEach(c => {
                if (c.name === currentVal) found = true;
                optionsHtml += `<option value="${c.name}">${c.name} (${c.port} : ${c.telnetPort})</option>`;
            });
            select.innerHTML = optionsHtml;
            if (found) {
                select.value = currentVal;
            } else {
                activeConsole = consoles[0].name;
                select.value = activeConsole;
            }

            // Update Telnet hint with real LAN host
            const activeObj = consoles.find(c => c.name === select.value) || consoles[0];
            if (activeObj) {
                const host = window.location.hostname || 'localhost';
                document.getElementById('telnetHint').textContent = `telnet ${host} ${activeObj.telnetPort}`;
            }
        }

        function onConsoleChanged(name) {
            activeConsole = name;
            connectWebSocket();
            pollActiveTarget();
        }

        async function updateTelemetryHistory() {
            if (!activeTarget) return;
            try {
                const res = await fetch(`/api/targets/${activeTarget}/mcu/telemetry/history?limit=30`);
                if (!res.ok) return;
                const data = await res.json();
                const history = data.history || [];
                if (history.length < 2) return;

                const temps = history.map(h => h.temperatureC).filter(t => t > 0);
                if (temps.length < 2) return;

                const minTemp = Math.min(...temps);
                const maxTemp = Math.max(...temps);
                const range = (maxTemp - minTemp) || 1.0;

                const width = 280;
                const height = 48;
                const padding = 6;

                let points = [];
                for (let i = 0; i < temps.length; i++) {
                    const x = (i / (temps.length - 1)) * width;
                    const normalized = (temps[i] - minTemp) / range;
                    const y = height - padding - (normalized * (height - 2 * padding));
                    points.push(`${x.toFixed(1)},${y.toFixed(1)}`);
                }

                const d = `M 0,${height} L ` + points.join(' L ') + ` L ${width},${height} Z`;
                document.getElementById('sparklinePath').setAttribute('d', d);
                document.getElementById('sparklineStats').textContent = `Min ${minTemp.toFixed(1)}°C / Max ${maxTemp.toFixed(1)}°C`;
            } catch (err) {
                // Ignore history fetch errors
            }
        }

        async function pollActiveTarget() {
            if (!activeTarget) return;
            try {
                const res = await fetch(`/api/targets/${activeTarget}`);
                if (!res.ok) return;
                const data = await res.json();

                document.getElementById('targetModel').textContent = data.model || '-';
                if (data.consoles && data.consoles.length > 0) {
                    document.getElementById('targetConsolesCount').textContent = `${data.consoles.length} port(s)`;
                    updateConsolesDropdown(data.consoles);
                } else if (data.console) {
                    document.getElementById('targetConsolesCount').textContent = data.console.port;
                    updateConsolesDropdown([data.console]);
                } else {
                    document.getElementById('targetConsolesCount').textContent = '-';
                    updateConsolesDropdown([]);
                }

                document.getElementById('targetMcu').textContent = (data.mcu ? `${data.mcu.port} (telnet ${data.mcu.telnetPort})` : '-');

                const powerBadge = document.getElementById('powerBadge');
                const isPowerOn = data.mcu && data.mcu.powerOn;
                if (isPowerOn) {
                    powerBadge.textContent = 'POWER ON';
                    powerBadge.style.background = 'rgba(16,185,129,0.15)';
                    powerBadge.style.color = 'var(--accent-emerald)';
                    powerBadge.style.borderColor = 'rgba(16,185,129,0.3)';
                } else {
                    powerBadge.textContent = 'POWER OFF';
                    powerBadge.style.background = 'rgba(244,63,94,0.15)';
                    powerBadge.style.color = 'var(--accent-rose)';
                    powerBadge.style.borderColor = 'rgba(244,63,94,0.3)';
                }

                if (data.mcu && data.mcu.latestSample) {
                    const sample = data.mcu.latestSample;
                    document.getElementById('statTemp').innerHTML = (sample.temperatureC > 0 ? sample.temperatureC.toFixed(1) : '--') + ' &deg;C';
                    document.getElementById('statFan').textContent = (sample.fanRpm > 0 ? sample.fanRpm : '--') + ' RPM';
                    document.getElementById('telemetryTime').textContent = sample.timestampIso || '';

                    const vContainer = document.getElementById('voltagesContainer');
                    if (sample.voltages && Object.keys(sample.voltages).length > 0) {
                        vContainer.innerHTML = '';
                        for (const [rail, val] of Object.entries(sample.voltages)) {
                            const item = document.createElement('div');
                            item.className = 'voltage-item';
                            item.innerHTML = `<span class="voltage-name">${rail}</span><span class="voltage-num">${val.toFixed(2)}V</span>`;
                            vContainer.appendChild(item);
                        }
                    }
                }

                updateTelemetryHistory();
            } catch (err) {
                console.error('Polling error', err);
            }
        }

        async function controlPower(action) {
            if (!activeTarget) return;
            try {
                await fetch(`/api/targets/${activeTarget}/mcu/power`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action })
                });
                setTimeout(pollActiveTarget, 400);
            } catch (err) {
                alert('Power control failed: ' + err);
            }
        }

        async function clearConsole() {
            if (term) {
                term.clear();
            }
            terminalLines = [''];
            scheduleTerminalRender();
            if (!activeTarget) return;
            try {
                const conParam = activeConsole ? `?console=${encodeURIComponent(activeConsole)}` : '';
                await fetch(`/api/targets/${activeTarget}/console/clear${conParam}`, { method: 'POST' });
            } catch (err) {
                console.error(err);
            }
        }

        // Initialize components on load
        initTerminal();
        fetchTargets();
        setInterval(pollActiveTarget, 2000);
    </script>
</body>
</html>
)rawhtml";
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
