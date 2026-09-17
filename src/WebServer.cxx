/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "TargetManager.hxx"
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
            } else {
                res.status = 400;
                res.set_content(R"({"error":"Invalid action, use 'on', 'off', or 'reboot'"})", "application/json");
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
        if (!target || !target->getConsole()) {
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
        j["lines"] = target->getConsole()->getRecentLines(lines);
        j["connected"] = target->getConsole()->isConnected();
        res.set_content(j.dump(2), "application/json");
    });

    // Write to console
    _server->Post(R"(/api/targets/([^/]+)/console/write)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target || !target->getConsole()) {
            res.status = 404;
            res.set_content(R"({"error":"Console not found for target"})", "application/json");
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string data = body.value("data", "");
            ssize_t written = target->getConsole()->write(data);
            nlohmann::json resp;
            resp["success"] = (written >= 0);
            resp["written"] = written;
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
        if (!target || !target->getConsole()) {
            res.status = 404;
            res.set_content(R"({"error":"Console not found for target"})", "application/json");
            return;
        }
        target->getConsole()->clearBuffer();
        res.set_content(R"({"success":true})", "application/json");
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
            padding: 1rem 2rem;
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
            padding: 1.5rem 2rem;
            max-width: 1600px;
            width: 100%;
            margin: 0 auto;
            display: flex;
            flex-direction: column;
            gap: 1.5rem;
        }
        .tabs-container {
            display: flex;
            gap: 0.5rem;
            border-bottom: 1px solid var(--border);
            padding-bottom: 0.5rem;
        }
        .tab-btn {
            background: transparent;
            border: 1px solid transparent;
            color: var(--text-muted);
            padding: 0.6rem 1.2rem;
            font-size: 0.95rem;
            font-weight: 600;
            border-radius: 6px;
            cursor: pointer;
            transition: all 0.2s;
            display: flex;
            align-items: center;
            gap: 0.5rem;
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
            grid-template-columns: 360px 1fr;
            gap: 1.5rem;
        }
        .card {
            background: var(--bg-card);
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 1.25rem;
            display: flex;
            flex-direction: column;
            gap: 1rem;
            box-shadow: 0 4px 20px rgba(0,0,0,0.15);
        }
        .card-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            border-bottom: 1px solid var(--border);
            padding-bottom: 0.75rem;
        }
        .card-title {
            font-size: 1rem;
            font-weight: 600;
            color: var(--text-main);
            display: flex;
            align-items: center;
            gap: 0.5rem;
        }
        .btn-group {
            display: flex;
            gap: 0.5rem;
        }
        .btn {
            padding: 0.5rem 1rem;
            border-radius: 6px;
            font-size: 0.85rem;
            font-weight: 600;
            border: none;
            cursor: pointer;
            transition: all 0.2s;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            gap: 0.4rem;
        }
        .btn-power-on { background: var(--accent-emerald); color: #022c22; }
        .btn-power-on:hover { filter: brightness(1.15); }
        .btn-power-off { background: var(--accent-rose); color: white; }
        .btn-power-off:hover { filter: brightness(1.15); }
        .btn-reboot { background: var(--accent-amber); color: #451a03; }
        .btn-reboot:hover { filter: brightness(1.15); }
        .btn-secondary { background: #22304d; color: var(--text-main); border: 1px solid var(--border); }
        .btn-secondary:hover { background: #2d3e63; }
        .meta-row {
            display: flex;
            justify-content: space-between;
            font-size: 0.85rem;
            padding: 0.35rem 0;
            border-bottom: 1px dashed rgba(255,255,255,0.06);
        }
        .meta-label { color: var(--text-muted); }
        .meta-val { font-family: monospace; font-weight: 600; }
        .telemetry-summary {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 0.75rem;
        }
        .stat-box {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 0.75rem;
            text-align: center;
        }
        .stat-label { font-size: 0.75rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.05em; }
        .stat-value { font-size: 1.4rem; font-weight: 700; color: var(--accent-cyan); margin-top: 0.2rem; }
        .voltages-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
            gap: 0.5rem;
            max-height: 240px;
            overflow-y: auto;
            padding-right: 0.25rem;
        }
        .voltage-item {
            background: #0d1424;
            border: 1px solid var(--border);
            padding: 0.4rem 0.6rem;
            border-radius: 6px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 0.8rem;
        }
        .voltage-name { color: var(--text-muted); font-size: 0.75rem; }
        .voltage-num { font-family: monospace; font-weight: 600; color: #38bdf8; }
        .terminal-container {
            flex: 1;
            display: flex;
            flex-direction: column;
            background: var(--bg-term);
            border: 1px solid var(--border);
            border-radius: 8px;
            overflow: hidden;
            min-height: 420px;
        }
        .terminal-topbar {
            background: #0f1629;
            border-bottom: 1px solid var(--border);
            padding: 0.5rem 1rem;
            display: flex;
            align-items: center;
            justify-content: space-between;
            font-size: 0.8rem;
            color: var(--text-muted);
        }
        .terminal-output {
            flex: 1;
            padding: 1rem;
            font-family: 'JetBrains Mono', Consolas, 'Courier New', monospace;
            font-size: 0.85rem;
            line-height: 1.45;
            color: #e2e8f0;
            overflow-y: auto;
            max-height: 520px;
            white-space: pre-wrap;
            word-break: break-all;
        }
        .cmd-bar {
            display: flex;
            gap: 0.5rem;
            padding: 0.75rem;
            background: #0f1629;
            border-top: 1px solid var(--border);
        }
        .cmd-input {
            flex: 1;
            background: var(--bg-term);
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 0.5rem 0.75rem;
            color: var(--text-main);
            font-family: monospace;
            font-size: 0.85rem;
            outline: none;
        }
        .cmd-input:focus { border-color: var(--accent-cyan); }
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
            <div style="display:flex; flex-direction:column; gap:1.5rem;">
                <div class="card">
                    <div class="card-header">
                        <span class="card-title">&#x26A1; Power & Target State</span>
                        <span id="powerBadge" class="status-pill" style="background:rgba(244,63,94,0.15); color:var(--accent-rose); border-color:rgba(244,63,94,0.3)">OFF</span>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-power-on" onclick="controlPower('on')">&#x25B6; Power ON</button>
                        <button class="btn btn-power-off" onclick="controlPower('off')">&#x25A0; Power OFF</button>
                        <button class="btn btn-reboot" onclick="controlPower('reboot')">&#x21BB; Reboot</button>
                    </div>
                    <div>
                        <div class="meta-row">
                            <span class="meta-label">Board Model</span>
                            <span id="targetModel" class="meta-val">-</span>
                        </div>
                        <div class="meta-row">
                            <span class="meta-label">Telnet Port</span>
                            <span id="targetTelnet" class="meta-val">-</span>
                        </div>
                        <div class="meta-row">
                            <span class="meta-label">Console Port</span>
                            <span id="targetConsole" class="meta-val">-</span>
                        </div>
                        <div class="meta-row">
                            <span class="meta-label">MCU Port</span>
                            <span id="targetMcu" class="meta-val">-</span>
                        </div>
                    </div>
                </div>

                <div class="card">
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
                    <div>
                        <div style="font-size:0.8rem; font-weight:600; color:var(--text-muted); margin-bottom:0.5rem;">Voltage Rails</div>
                        <div id="voltagesContainer" class="voltages-grid">
                            <span style="color:var(--text-muted); font-size:0.8rem;">Waiting for telemetry...</span>
                        </div>
                    </div>
                </div>

                <div class="card">
                    <div class="card-header">
                        <span class="card-title">&#x2328; MCU Direct Command</span>
                    </div>
                    <div style="display:flex; gap:0.5rem;">
                        <input id="mcuCmdInput" class="cmd-input" type="text" placeholder="e.g. bs, version, help" onkeydown="if(event.key==='Enter') sendMcuCommand()">
                        <button class="btn btn-secondary" onclick="sendMcuCommand()">Send</button>
                    </div>
                    <div id="mcuCmdResp" style="font-family:monospace; font-size:0.8rem; background:#0d1424; padding:0.5rem; border-radius:4px; max-height:120px; overflow-y:auto; color:#94a3b8; white-space:pre-wrap;">(MCU response will appear here)</div>
                </div>
            </div>

            <!-- Right Column: Live Serial Console -->
            <div class="card" style="min-height:600px;">
                <div class="card-header">
                    <span class="card-title">&#x1F4BB; Serial Console Output</span>
                    <div style="display:flex; align-items:center; gap:0.5rem;">
                        <span id="telnetHint" style="font-family:monospace; font-size:0.8rem; color:var(--accent-cyan); background:#0d1424; padding:0.2rem 0.5rem; border-radius:4px;">telnet localhost 3887</span>
                        <button class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.75rem;" onclick="clearConsole()">Clear</button>
                    </div>
                </div>
                <div class="terminal-container">
                    <div class="terminal-topbar">
                        <span id="consoleStats">0 lines buffered</span>
                        <label style="display:flex; align-items:center; gap:0.4rem; cursor:pointer;">
                            <input id="autoScrollCheck" type="checkbox" checked> Auto-scroll
                        </label>
                    </div>
                    <div id="consoleText" class="terminal-output">Connecting to target console...</div>
                    <div class="cmd-bar">
                        <input id="consoleInput" class="cmd-input" type="text" placeholder="Send input to console..." onkeydown="if(event.key==='Enter') sendConsoleInput()">
                        <button class="btn btn-secondary" onclick="sendConsoleInput()">Send</button>
                    </div>
                </div>
            </div>
        </div>
    </main>

    <script>
        let targets = [];
        let activeTarget = '';

        function updateClock() {
            const now = new Date();
            document.getElementById('clockDisplay').textContent = now.toTimeString().split(' ')[0];
        }
        setInterval(updateClock, 1000);
        updateClock();

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
            renderTabs();
            pollActiveTarget();
        }

        async function pollActiveTarget() {
            if (!activeTarget) return;
            try {
                const res = await fetch(`/api/targets/${activeTarget}`);
                if (!res.ok) return;
                const data = await res.json();

                document.getElementById('targetModel').textContent = data.model || '-';
                document.getElementById('targetTelnet').textContent = data.telnetPort || '-';
                document.getElementById('targetConsole').textContent = (data.console ? data.console.port : '-');
                document.getElementById('targetMcu').textContent = (data.mcu ? data.mcu.port : '-');
                document.getElementById('telnetHint').textContent = `telnet localhost ${data.telnetPort || 3887}`;

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

                // Fetch console
                const conRes = await fetch(`/api/targets/${activeTarget}/console?lines=100`);
                if (conRes.ok) {
                    const conData = await conRes.json();
                    const term = document.getElementById('consoleText');
                    const lines = conData.lines || [];
                    document.getElementById('consoleStats').textContent = `${lines.length} lines buffered`;
                    term.textContent = lines.join('\n');
                    if (document.getElementById('autoScrollCheck').checked) {
                        term.scrollTop = term.scrollHeight;
                    }
                }
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
                setTimeout(pollActiveTarget, 300);
            } catch (err) {
                alert('Power control failed: ' + err);
            }
        }

        async function sendMcuCommand() {
            const input = document.getElementById('mcuCmdInput');
            const cmd = input.value.trim();
            if (!cmd || !activeTarget) return;
            input.value = '';
            try {
                const res = await fetch(`/api/targets/${activeTarget}/mcu/command`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: cmd })
                });
                const data = await res.json();
                document.getElementById('mcuCmdResp').textContent = data.response || '(no response)';
                setTimeout(pollActiveTarget, 300);
            } catch (err) {
                document.getElementById('mcuCmdResp').textContent = 'Error: ' + err;
            }
        }

        async function sendConsoleInput() {
            const input = document.getElementById('consoleInput');
            const data = input.value;
            if (!data || !activeTarget) return;
            input.value = '';
            try {
                await fetch(`/api/targets/${activeTarget}/console/write`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ data: data + '\n' })
                });
                setTimeout(pollActiveTarget, 200);
            } catch (err) {
                console.error(err);
            }
        }

        async function clearConsole() {
            if (!activeTarget) return;
            try {
                await fetch(`/api/targets/${activeTarget}/console/clear`, { method: 'POST' });
                document.getElementById('consoleText').textContent = '';
                document.getElementById('consoleStats').textContent = '0 lines buffered';
            } catch (err) {
                console.error(err);
            }
        }

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
