/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "TargetManager.hxx"
#include "FlashManager.hxx"
#include "UsageDatabase.hxx"
#include "TerminalSanitizer.hxx"
#include "Config.hxx"
#include "XyModem.hxx"
#include "ToolDispatcher.hxx"
#include "Version.hxx"
#include <sys/stat.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>

namespace embdevenv {

static std::string generateMcpSessionId()
{
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;

    uint64_t p1 = dis(gen);
    uint64_t p2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << p1 << std::setw(16) << p2;
    return oss.str();
}

WebServer::WebServer(int port, const std::string &bindAddress, bool endpointsEnabled)
    : _port(port)
    , _bindAddress(bindAddress)
    , _endpointsEnabled(endpointsEnabled)
    , _running(false)
{
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::areEndpointsEnabled() const
{
    return _endpointsEnabled;
}

void WebServer::setEndpointsEnabled(bool enabled)
{
    _endpointsEnabled = enabled;
}

bool WebServer::start()
{
    if (_running) {
        return true;
    }

    _server = std::make_unique<httplib::Server>();
    setupRoutes();

    if (_port == 0) {
        int boundPort = _server->bind_to_any_port(_bindAddress.c_str());
        if (boundPort <= 0) {
            fprintf(stderr, "[embdevenv] WebServer error: failed to bind to any port on %s\n",
                    _bindAddress.c_str());
            _server.reset();
            return false;
        }
        _port = boundPort;
    } else {
        if (!_server->bind_to_port(_bindAddress.c_str(), _port)) {
            fprintf(stderr, "[embdevenv] WebServer error: failed to bind to %s:%d\n",
                    _bindAddress.c_str(), _port);
            _server.reset();
            return false;
        }
    }

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

    {
        std::lock_guard<std::mutex> lock(_mcpMutex);
        for (auto &kv : _mcpSessions) {
            kv.second.closed = true;
        }
    }
    _mcpCv.notify_all();

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
        if (!_server->listen_after_bind()) {
            fprintf(stderr, "[embdevenv] WebServer error: failed to listen on %s:%d\n",
                    _bindAddress.c_str(), _port);
        }
    }
    _running = false;
}

std::string WebServer::createUiSession()
{
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;

    uint64_t p1 = dis(gen);
    uint64_t p2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << p1 << std::setw(16) << p2;
    std::string token = oss.str();

    std::lock_guard<std::mutex> lock(_sessionMutex);
    time_t now = time(nullptr);
    for (auto it = _uiSessions.begin(); it != _uiSessions.end(); ) {
        if (now - it->second > 86400 * 7) {
            it = _uiSessions.erase(it);
        } else {
            ++it;
        }
    }
    _uiSessions[token] = now;
    return token;
}

bool WebServer::isValidUiSession(const httplib::Request &req) const
{
    // 1. Block command-line agents and automated scripts
    if (req.has_header("User-Agent")) {
        std::string ua = req.get_header_value("User-Agent");
        for (char &c : ua) c = tolower(c);
        if (ua.find("curl/") != std::string::npos ||
            ua.find("python") != std::string::npos ||
            ua.find("wget/") != std::string::npos ||
            ua.find("httpie") != std::string::npos ||
            ua.find("aiohttp") != std::string::npos ||
            ua.find("go-http-client") != std::string::npos) {
            return false;
        }
    } else {
        return false;
    }

    // 2. Extract session token
    std::string token;
    if (req.has_header("X-UI-Session")) {
        token = req.get_header_value("X-UI-Session");
    } else if (req.has_header("Cookie")) {
        std::string cookie = req.get_header_value("Cookie");
        size_t pos = cookie.find("embdevenv_session=");
        if (pos != std::string::npos) {
            size_t start = pos + 18;
            size_t end = cookie.find(';', start);
            token = (end == std::string::npos) ? cookie.substr(start) : cookie.substr(start, end - start);
        }
    }

    if (token.empty()) {
        return false;
    }

    // 3. Verify token exists and is fresh
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _uiSessions.find(token);
    if (it != _uiSessions.end()) {
        time_t now = time(nullptr);
        if (now - it->second < 86400 * 7) {
            return true;
        }
    }

    return false;
}

static std::string getMcpHintForPath(const std::string &path, const std::string &body)
{
    // Extract target name if present: /api/targets/<target>/...
    std::string target;
    if (path.rfind("/api/targets/", 0) == 0) {
        size_t start = 13;
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            target = path.substr(start);
        } else {
            target = path.substr(start, end - start);
        }
    }

    if (path == "/api/status") {
        return "Use official MCP tool 'embdevenv_get_target_status' or 'embdevenv_list_targets'.";
    }
    if (path == "/api/targets") {
        return "Use official MCP tool 'embdevenv_list_targets'.";
    }
    if (path == "/api/usb/devices") {
        return "Use official MCP tool 'embdevenv_usb_list_devices'.";
    }
    if (path.find("/mcu/power") != std::string::npos) {
        if (!body.empty()) {
            try {
                auto j = nlohmann::json::parse(body);
                std::string action = j.value("action", "");
                if (action == "on") {
                    return "Use official MCP tool 'embdevenv_mcu_power' with argument {\"target\": \"" + target + "\", \"action\": \"power_on\"}.";
                } else if (action == "off") {
                    return "Use official MCP tool 'embdevenv_mcu_power' with argument {\"target\": \"" + target + "\", \"action\": \"power_off\"}.";
                } else if (action == "reboot") {
                    return "Use official MCP tool 'embdevenv_mcu_power' with argument {\"target\": \"" + target + "\", \"action\": \"reboot\"}.";
                }
            } catch (...) {}
        }
        return "Use official MCP tool 'embdevenv_mcu_power' with argument {\"target\": \"" + target + "\", \"action\": \"power_on\"|\"power_off\"|\"reboot\"}.";
    }
    if (path.find("/mcu/command") != std::string::npos) {
        return "Direct raw MCU commands are disabled and prohibited.";
    }
    if (path.find("/mcu/telemetry/history") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_mcu_get_telemetry_history' with argument {\"target\": \"" + target + "\"}.";
    }
    if (path.find("/mcu/telemetry") != std::string::npos || path.find("/mcu") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_mcu_get_telemetry' with argument {\"target\": \"" + target + "\", \"metric\": \"all\"|\"voltages\"|\"thermal\"|\"fan\"|\"power\"}.";
    }
    if (path.find("/console/write") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_console_write' or 'embdevenv_console_expect' with argument {\"target\": \"" + target + "\", \"command\": \"<command>\"}.";
    }
    if (path.find("/console/load") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_console_load' with argument {\"target\": \"" + target + "\", \"file_path\": \"<path>\"}.";
    }
    if (path.find("/console/clear") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_console_clear' with argument {\"target\": \"" + target + "\"}.";
    }
    if (path.find("/console") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_console_read', 'embdevenv_console_expect', or 'embdevenv_console_catch_bootloader' with argument {\"target\": \"" + target + "\"}.";
    }
    if (path.find("/flash/status") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_flash_get_status' with argument {\"target\": \"" + target + "\"}.";
    }
    if (path.find("/flash") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_flash_target' with argument {\"target\": \"" + target + "\"}.";
    }
    if (path.find("/stats") != std::string::npos) {
        return "Use official MCP tool 'embdevenv_get_board_stats' with argument {\"target\": \"" + target + "\"}.";
    }
    if (!target.empty()) {
        return "Use official MCP tool 'embdevenv_get_target_status' with argument {\"target\": \"" + target + "\"}.";
    }
    return "Use official MCP tools ('embdevenv_*' via aimon gateway or standalone server). Direct REST API access is disabled.";
}

void WebServer::setupRoutes()
{
    if (_server == nullptr) {
        return;
    }

    // Gate REST API endpoints if disabled by configuration, unless request is from an authenticated Web UI session
    if (!_endpointsEnabled) {
        _server->set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) {
                if (!isValidUiSession(req)) {
                    res.status = 403;
                    std::string mcpHint = getMcpHintForPath(req.path, req.body);
                    nlohmann::json err = {
                        {"error", "Direct REST API access is disabled. Use official MCP tools instead."},
                        {"hint", mcpHint}
                    };
                    res.set_content(err.dump(2), "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    // Root dashboard
    _server->Get("/", [this](const httplib::Request &, httplib::Response &res) {
        std::string token = createUiSession();
        res.set_header("Set-Cookie", "embdevenv_session=" + token + "; Path=/; SameSite=Strict");
        res.set_content(getDashboardHtml(token), "text/html");
    });

    auto setCorsHeaders = [](httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       "Content-Type, Accept, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Last-Event-ID");
        res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");
    };

    auto handleOptions = [setCorsHeaders](const httplib::Request &, httplib::Response &res) {
        setCorsHeaders(res);
        res.status = 204;
    };
    _server->Options("/mcp", handleOptions);
    _server->Options("/sse", handleOptions);
    _server->Options("/message", handleOptions);

    auto handleStreamableHttpPost = [this, setCorsHeaders](const httplib::Request &req, httplib::Response &res) {
        setCorsHeaders(res);

        std::string sessionId;
        if (req.has_header("Mcp-Session-Id")) {
            sessionId = req.get_header_value("Mcp-Session-Id");
        }

        bool isInit = false;
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.is_object() && j.value("method", "") == "initialize") {
                isInit = true;
            }
        } catch (...) {}

        time_t now = time(nullptr);
        if (sessionId.empty() && isInit) {
            sessionId = generateMcpSessionId();
        }

        if (!sessionId.empty()) {
            std::lock_guard<std::mutex> lock(_mcpMutex);
            auto it = _mcpSessions.find(sessionId);
            if (it != _mcpSessions.end()) {
                it->second.lastActivity = now;
            } else {
                McpSessionState state;
                state.sessionId = sessionId;
                state.lastActivity = now;
                state.closed = false;
                _mcpSessions[sessionId] = std::move(state);
            }
            res.set_header("Mcp-Session-Id", sessionId);
        }

        std::string resp = ToolDispatcher::handleJsonRpc(req.body, "2025-03-26");
        if (resp.find("\"code\":-32700") != std::string::npos || resp.find("\"code\": -32700") != std::string::npos) {
            res.status = 400;
            res.set_content(resp, "application/json");
            return;
        }

        if (resp.empty()) {
            res.status = 202;
            res.body.clear();
        } else {
            res.status = 200;
            res.set_content(resp, "application/json");
        }
    };

    _server->Post("/mcp", handleStreamableHttpPost);
    _server->Post("/sse", handleStreamableHttpPost);

    _server->Post("/message", [setCorsHeaders](const httplib::Request &req, httplib::Response &res) {
        setCorsHeaders(res);
        std::string resp = ToolDispatcher::handleJsonRpc(req.body, "2024-11-05");
        if (resp.find("\"code\":-32700") != std::string::npos || resp.find("\"code\": -32700") != std::string::npos) {
            res.status = 400;
            res.set_content(resp, "application/json");
            return;
        }
        if (resp.empty()) {
            res.status = 202;
            res.body.clear();
        } else {
            res.status = 200;
            res.set_content(resp, "application/json");
        }
    });

    _server->Get("/sse", [setCorsHeaders](const httplib::Request &, httplib::Response &res) {
        setCorsHeaders(res);
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_content("event: endpoint\ndata: /message\n\n", "text/event-stream");
    });

    _server->Get("/mcp", [this, setCorsHeaders](const httplib::Request &req, httplib::Response &res) {
        setCorsHeaders(res);

        if (req.has_header("Accept")) {
            std::string accept = req.get_header_value("Accept");
            if (accept.find("text/event-stream") == std::string::npos &&
                accept.find("*/*") == std::string::npos) {
                res.status = 405;
                return;
            }
        }

        std::string sessionId;
        if (req.has_header("Mcp-Session-Id")) {
            sessionId = req.get_header_value("Mcp-Session-Id");
        }

        time_t now = time(nullptr);
        if (sessionId.empty()) {
            sessionId = generateMcpSessionId();
        }

        {
            std::lock_guard<std::mutex> lock(_mcpMutex);
            auto it = _mcpSessions.find(sessionId);
            if (it != _mcpSessions.end()) {
                it->second.lastActivity = now;
            } else {
                McpSessionState state;
                state.sessionId = sessionId;
                state.lastActivity = now;
                state.closed = false;
                _mcpSessions[sessionId] = std::move(state);
            }
        }

        res.set_header("Mcp-Session-Id", sessionId);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        auto lastKeepalive = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto initialSent = std::make_shared<bool>(false);

        auto provider = [this, sessionId, lastKeepalive, initialSent](size_t offset, httplib::DataSink &sink) -> bool {
            (void) offset;
            if (!sink.is_writable() || !_running) {
                return false;
            }

            if (!*initialSent) {
                *initialSent = true;
                std::string connMsg = ": connected\n\n";
                return sink.write(connMsg.data(), connMsg.size());
            }

            std::vector<std::string> toSend;
            {
                std::unique_lock<std::mutex> lock(_mcpMutex);
                _mcpCv.wait_for(lock, std::chrono::milliseconds(200), [this, &sessionId]() {
                    if (!_running) return true;
                    auto it = _mcpSessions.find(sessionId);
                    if (it == _mcpSessions.end() || it->second.closed) return true;
                    return !it->second.pendingMessages.empty();
                });

                if (!_running) {
                    return false;
                }

                auto it = _mcpSessions.find(sessionId);
                if (it == _mcpSessions.end() || it->second.closed) {
                    return false;
                }

                it->second.lastActivity = time(nullptr);

                if (!it->second.pendingMessages.empty()) {
                    toSend = std::move(it->second.pendingMessages);
                    it->second.pendingMessages.clear();
                }
            }

            if (!toSend.empty()) {
                std::string batch;
                for (const auto &msg : toSend) {
                    batch += "event: message\ndata: " + msg + "\n\n";
                }
                *lastKeepalive = std::chrono::steady_clock::now();
                return sink.write(batch.data(), batch.size());
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - *lastKeepalive).count() >= 15) {
                *lastKeepalive = now;
                std::string ping = ": keepalive\n\n";
                return sink.write(ping.data(), ping.size());
            }

            return true;
        };

        auto releaser = [this, sessionId](bool success) {
            (void) success;
            std::lock_guard<std::mutex> lock(_mcpMutex);
            auto it = _mcpSessions.find(sessionId);
            if (it != _mcpSessions.end()) {
                it->second.lastActivity = time(nullptr);
            }
        };

        res.set_chunked_content_provider("text/event-stream", provider, releaser);
    });

    _server->Delete("/mcp", [this, setCorsHeaders](const httplib::Request &req, httplib::Response &res) {
        setCorsHeaders(res);
        if (req.has_header("Mcp-Session-Id")) {
            std::string sessionId = req.get_header_value("Mcp-Session-Id");
            {
                std::lock_guard<std::mutex> lock(_mcpMutex);
                auto it = _mcpSessions.find(sessionId);
                if (it != _mcpSessions.end()) {
                    it->second.closed = true;
                    _mcpSessions.erase(it);
                }
            }
            _mcpCv.notify_all();
        }
        res.status = 204;
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

    // Target board usage statistics
    _server->Get(R"(/api/targets/([^/]+)/stats)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }
        int windowDays = 30;
        if (req.has_param("window")) {
            try {
                windowDays = std::stoi(req.get_param_value("window"));
            } catch (...) {
            }
        }
        nlohmann::json stats = UsageDatabase::getInstance().queryBoardStats(name, windowDays);
        res.set_content(stats.dump(2), "application/json");
    });

    // Target board recent lifecycle events
    _server->Get(R"(/api/targets/([^/]+)/events)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {
            }
        }
        nlohmann::json events = UsageDatabase::getInstance().queryRecentEvents(name, limit);
        res.set_content(events.dump(2), "application/json");
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

            data = TerminalSanitizer::sanitize(data);
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

    // Console load firmware via YMODEM/XMODEM
    _server->Post(R"(/api/targets/([^/]+)/console/load)", [](const httplib::Request &req, httplib::Response &res) {
        std::string name = req.matches[1];
        auto target = TargetManager::getInstance().getTarget(name);
        if (!target) {
            res.status = 404;
            res.set_content(R"({"error":"Target not found"})", "application/json");
            return;
        }

        try {
            nlohmann::json body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            std::string filePath = body.value("file_path", "");
            if (filePath.empty()) {
                filePath = body.value("file", "");
            }
            std::string protocol = body.value("protocol", "ymodem");
            std::string address = body.value("address", "0x20000000");
            bool sendCommand = body.value("send_command", true);
            int timeoutSec = body.value("timeout", 300);
            if (body.contains("timeout_sec")) {
                timeoutSec = body.value("timeout_sec", 300);
            }
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

            if (filePath.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"'file_path' argument is required"})", "application/json");
                return;
            }

            if (console->isTransferInProgress()) {
                res.status = 409;
                res.set_content(R"({"error":"Console is currently locked for active serial file transfer"})", "application/json");
                return;
            }

            std::string resolvedPath = Config::resolveHomePath(filePath);
            struct stat st;
            if (stat(resolvedPath.c_str(), &st) != 0) {
                res.status = 400;
                nlohmann::json err = {{"error", "File not found: " + resolvedPath}};
                res.set_content(err.dump(), "application/json");
                return;
            }

            XyModem::Protocol proto = XyModem::Protocol::YMODEM;
            std::string protoLower = protocol;
            std::transform(protoLower.begin(), protoLower.end(), protoLower.begin(), ::tolower);
            if (protoLower == "loadx" || protoLower == "xmodem") {
                proto = XyModem::Protocol::XMODEM;
            }

            auto result = console->transferFile(resolvedPath, proto, sendCommand, address, timeoutSec);

            UsageDatabase::getInstance().recordEvent(
                name,
                proto == XyModem::Protocol::YMODEM ? "console_loady" : "console_loadx",
                result.success ? "success" : "failed",
                static_cast<int64_t>(result.elapsedSec * 1000.0),
                result.errorMessage.empty() ? (std::to_string(result.bytesSent) + " bytes") : result.errorMessage
            );

            nlohmann::json j = {
                {"target", name},
                {"console", console->getName()},
                {"success", result.success},
                {"protocol", result.protocol},
                {"fileSize", result.fileSize},
                {"bytesSent", result.bytesSent},
                {"packetsSent", result.packetsSent},
                {"retries", result.retries},
                {"elapsedSec", result.elapsedSec},
                {"transferRateKBps", result.transferRateKBps},
                {"capturedOutput", result.capturedOutput}
            };
            if (!result.success && !result.errorMessage.empty()) {
                j["errorMessage"] = result.errorMessage;
            }
            res.status = result.success ? 200 : 500;
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 400;
            nlohmann::json err = {{"error", ex.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // List host USB devices via usb-matrix -l
    _server->Get("/api/usb/devices", [](const httplib::Request &, httplib::Response &res) {
        auto j = FlashManager::getInstance().listUsbDevices();
        res.set_content(j.dump(2), "application/json");
    });

    // Browse host filesystem for firmware / binary selection
    _server->Get("/api/fs/browse", [](const httplib::Request &req, httplib::Response &res) {
        namespace fs = std::filesystem;
        try {
            std::string reqPath = req.get_param_value("path");
            std::string extFilter = req.get_param_value("ext");

            // Expand tilde if present
            if (!reqPath.empty() && reqPath[0] == '~') {
                const char *home = getenv("HOME");
                if (home) {
                    reqPath = std::string(home) + reqPath.substr(1);
                }
            }

            fs::path targetPath;
            if (reqPath.empty()) {
                const char *home = getenv("HOME");
                if (home && fs::exists(fs::path(home) / "work")) {
                    targetPath = fs::path(home) / "work";
                } else if (home && fs::exists(home)) {
                    targetPath = fs::path(home);
                } else {
                    targetPath = fs::current_path();
                }
            } else {
                targetPath = fs::path(reqPath);
            }

            std::error_code ec;
            targetPath = fs::weakly_canonical(targetPath, ec);
            if (ec || !fs::exists(targetPath, ec)) {
                if (targetPath.has_parent_path() && fs::exists(targetPath.parent_path(), ec)) {
                    targetPath = targetPath.parent_path();
                } else {
                    const char *home = getenv("HOME");
                    targetPath = home ? fs::path(home) : fs::current_path();
                }
            } else if (!fs::is_directory(targetPath, ec)) {
                targetPath = targetPath.parent_path();
            }

            // Parse extension filters (e.g. ".bin,.pkg,.img" or "bin,pkg,img")
            std::vector<std::string> exts;
            if (!extFilter.empty()) {
                std::stringstream ss(extFilter);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    token.erase(0, token.find_first_not_of(" \t"));
                    token.erase(token.find_last_not_of(" \t") + 1);
                    if (!token.empty()) {
                        if (token[0] != '.') token = "." + token;
                        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
                        exts.push_back(token);
                    }
                }
            }

            nlohmann::json entries = nlohmann::json::array();
            fs::directory_iterator dirIt(targetPath, fs::directory_options::skip_permission_denied, ec);
            if (!ec) {
                struct EntryInfo {
                    std::string name;
                    std::string path;
                    bool isDir;
                    int64_t size;
                    int64_t mtime;
                };
                std::vector<EntryInfo> list;

                for (const auto &entry : dirIt) {
                    std::error_code entryEc;
                    auto status = entry.status(entryEc);
                    if (entryEc) continue;

                    bool isDir = fs::is_directory(status);
                    std::string filename = entry.path().filename().string();
                    if (filename.empty() || filename[0] == '.') {
                        continue; // Skip hidden files/directories
                    }

                    if (!isDir && !exts.empty()) {
                        std::string entryExt = entry.path().extension().string();
                        std::transform(entryExt.begin(), entryExt.end(), entryExt.begin(), ::tolower);
                        bool match = false;
                        for (const auto &e : exts) {
                            if (entryExt == e) { match = true; break; }
                        }
                        if (!match) continue;
                    }

                    int64_t sz = 0;
                    if (!isDir) {
                        sz = static_cast<int64_t>(entry.file_size(entryEc));
                        if (entryEc) sz = 0;
                    }

                    int64_t mtimeSec = 0;
                    struct stat st;
                    if (stat(entry.path().c_str(), &st) == 0) {
                        mtimeSec = st.st_mtime;
                    }

                    list.push_back({filename, entry.path().string(), isDir, sz, mtimeSec});
                }

                // Sort: directories first (alphabetical), then files (alphabetical)
                std::sort(list.begin(), list.end(), [](const EntryInfo &a, const EntryInfo &b) {
                    if (a.isDir != b.isDir) return a.isDir > b.isDir;
                    return a.name < b.name;
                });

                for (const auto &item : list) {
                    entries.push_back({
                        {"name", item.name},
                        {"path", item.path},
                        {"is_dir", item.isDir},
                        {"size", item.size},
                        {"mtime", item.mtime}
                    });
                }
            }

            std::string parentStr = "";
            if (targetPath.has_parent_path() && targetPath != targetPath.parent_path()) {
                parentStr = targetPath.parent_path().string();
            }

            nlohmann::json resp;
            resp["current"] = targetPath.string();
            resp["parent"] = parentStr;
            resp["entries"] = entries;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception &ex) {
            res.status = 500;
            nlohmann::json err = {{"error", ex.what()}};
            res.set_content(err.dump(), "application/json");
        }
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
    _server->WebSocket(R"(/ws/targets/([^/]+)/console)", [this](const httplib::Request &req, httplib::ws::WebSocket &ws) {
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

        // Hydrate initial terminal state with recent buffer history:
        // Stream completed lines with trailing \r\n, and stream the active
        // partial prompt line WITHOUT \r\n to preserve inline cursor positioning.
        auto completed = console->getCompletedLines(100);
        for (const auto &line : completed) {
            ws.send(line + "\r\n");
        }
        std::string partial = console->getPartialLine();
        if (!partial.empty()) {
            ws.send(partial);
        }

        // Stream live serial UART output directly to WebSocket
        auto wsSendMutex = std::make_shared<std::mutex>();
        int listenerId = console->addListener([&ws, wsSendMutex](const char *data, size_t len) {
            std::lock_guard<std::mutex> lock(*wsSendMutex);
            if (ws.is_open()) {
                ws.send(std::string(data, len));
            }
        });

        // Set timeout to 1 second so loop can check _running periodically
        ws.set_read_timeout(1);

        TerminalSanitizer wsSanitizer;
        std::string msg;
        while (_running.load()) {
            auto res = ws.read(msg);
            if (res == httplib::ws::ReadResult::Fail) {
                break;
            }
            if (res == httplib::ws::ReadResult::Timeout) {
                continue;
            }
            if (!msg.empty()) {
                std::string cleanMsg = wsSanitizer.process(msg);
                if (cleanMsg.empty()) {
                    continue;
                }
                if (console->isTransferInProgress()) {
                    if (cleanMsg.find('\x03') != std::string::npos) {
                        console->cancelTransfer();
                        std::lock_guard<std::mutex> lock(*wsSendMutex);
                        if (ws.is_open()) {
                            ws.send("\r\n[embdevenv] Serial transfer aborted by user.\r\n");
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(*wsSendMutex);
                        if (ws.is_open()) {
                            ws.send("\r\n[embdevenv] Serial transfer in progress. Input locked (Ctrl-C to abort).\r\n");
                        }
                    }
                } else {
                    console->write(cleanMsg);
                }
            }
        }

        ws.close();
        console->removeListener(listenerId);
    });
}

std::string WebServer::getDashboardHtml(const std::string &sessionToken) const
{
    std::string html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>embdevenv - Embedded Target Dashboard</title>
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.min.css">
    <script src="https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.min.js"></script>
    <script>
        window.__UI_SESSION_TOKEN__ = ")rawhtml";
    html += sessionToken;
    html += R"rawhtml(";
        (function() {
            const originalFetch = window.fetch;
            window.fetch = function(url, options) {
                options = options || {};
                options.headers = options.headers || {};
                if (window.__UI_SESSION_TOKEN__) {
                    if (options.headers instanceof Headers) {
                        options.headers.set('X-UI-Session', window.__UI_SESSION_TOKEN__);
                    } else if (Array.isArray(options.headers)) {
                        options.headers.push(['X-UI-Session', window.__UI_SESSION_TOKEN__]);
                    } else {
                        options.headers['X-UI-Session'] = window.__UI_SESSION_TOKEN__;
                    }
                }
                return originalFetch(url, options);
            };
        })();
    </script>
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
            grid-template-columns: 360px 1fr;
            gap: 1rem;
            align-items: start;
        }
        @media (max-width: 900px) {
            .dashboard-grid {
                grid-template-columns: 1fr;
            }
        }
        /* Board Statistics GUI Card */
        .board-stats-card {
            background: var(--bg-card);
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 1rem;
            display: flex;
            flex-direction: column;
            gap: 1rem;
            box-shadow: 0 4px 20px rgba(0,0,0,0.15);
            margin-top: 0.25rem;
        }
        .stats-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 0.75rem;
            flex-wrap: wrap;
            border-bottom: 1px solid var(--border);
            padding-bottom: 0.75rem;
        }
        .stats-window-group {
            display: inline-flex;
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 2px;
            gap: 2px;
        }
        .window-btn {
            background: transparent;
            border: none;
            color: var(--text-muted);
            padding: 0.25rem 0.65rem;
            font-size: 0.75rem;
            font-weight: 600;
            border-radius: 4px;
            cursor: pointer;
            transition: all 0.15s ease;
        }
        .window-btn:hover {
            color: var(--text-main);
            background: rgba(255,255,255,0.05);
        }
        .window-btn.active {
            color: var(--accent-cyan);
            background: rgba(6,182,212,0.15);
            box-shadow: 0 1px 4px rgba(0,0,0,0.2);
        }
        .stats-kpi-grid {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 0.75rem;
        }
        @media (max-width: 1024px) {
            .stats-kpi-grid {
                grid-template-columns: repeat(2, 1fr);
            }
        }
        @media (max-width: 600px) {
            .stats-kpi-grid {
                grid-template-columns: 1fr;
            }
        }
        .stat-kpi-card {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 0.85rem;
            display: flex;
            flex-direction: column;
            gap: 0.4rem;
            transition: border-color 0.2s;
        }
        .stat-kpi-card:hover {
            border-color: rgba(6,182,212,0.4);
        }
        .stat-kpi-label {
            font-size: 0.75rem;
            font-weight: 600;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 0.05em;
            display: flex;
            align-items: center;
            gap: 0.4rem;
        }
        .stat-kpi-val {
            font-size: 1.6rem;
            font-weight: 700;
            font-variant-numeric: tabular-nums;
            color: var(--text-main);
            line-height: 1.2;
        }
        .stat-kpi-sub {
            font-size: 0.75rem;
            color: var(--text-muted);
            display: flex;
            align-items: center;
            gap: 0.5rem;
            flex-wrap: wrap;
        }
        .stats-visual-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 0.75rem;
        }
        @media (max-width: 768px) {
            .stats-visual-grid {
                grid-template-columns: 1fr;
            }
        }
        .health-card {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 0.85rem;
            display: flex;
            flex-direction: column;
            gap: 0.5rem;
        }
        .health-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
        }
        .health-bar-track {
            height: 8px;
            background: rgba(255,255,255,0.06);
            border-radius: 9999px;
            overflow: hidden;
            position: relative;
        }
        .health-bar-fill {
            height: 100%;
            background: linear-gradient(90deg, var(--accent-cyan), var(--accent-emerald));
            border-radius: 9999px;
            transition: width 0.4s ease;
        }
        .health-footer {
            display: flex;
            align-items: center;
            justify-content: space-between;
            font-size: 0.72rem;
            color: var(--text-muted);
        }
        .temp-envelope-track {
            height: 8px;
            background: linear-gradient(90deg, #10b981 0%, #06b6d4 50%, #f59e0b 75%, #f43f5e 100%);
            border-radius: 9999px;
            position: relative;
            opacity: 0.85;
        }
        .temp-envelope-bar {
            position: absolute;
            top: -2px;
            bottom: -2px;
            background: white;
            border-radius: 4px;
            box-shadow: 0 0 6px rgba(255,255,255,0.8);
            transition: all 0.4s ease;
            width: 8px;
            left: 50%;
        }
        .events-container {
            background: #0d1424;
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 0.85rem;
            display: flex;
            flex-direction: column;
            gap: 0.6rem;
        }
        .events-toolbar {
            display: flex;
            align-items: center;
            justify-content: space-between;
            flex-wrap: wrap;
            gap: 0.5rem;
        }
        .events-filter-group {
            display: flex;
            gap: 0.35rem;
        }
        .filter-chip {
            background: transparent;
            border: 1px solid var(--border);
            color: var(--text-muted);
            padding: 0.2rem 0.55rem;
            font-size: 0.72rem;
            font-weight: 600;
            border-radius: 4px;
            cursor: pointer;
            transition: all 0.15s;
        }
        .filter-chip:hover {
            color: var(--text-main);
            border-color: rgba(255,255,255,0.2);
        }
        .filter-chip.active {
            color: var(--accent-cyan);
            background: rgba(6,182,212,0.12);
            border-color: var(--accent-cyan);
        }
        .events-table-wrapper {
            max-height: 260px;
            overflow-y: auto;
            border: 1px solid var(--border);
            border-radius: 6px;
        }
        .events-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.78rem;
            text-align: left;
        }
        .events-table th {
            background: #111827;
            color: var(--text-muted);
            font-weight: 600;
            padding: 0.45rem 0.65rem;
            border-bottom: 1px solid var(--border);
            position: sticky;
            top: 0;
            z-index: 1;
        }
        .events-table td {
            padding: 0.4rem 0.65rem;
            border-bottom: 1px solid rgba(35,47,72,0.4);
            color: var(--text-main);
            font-variant-numeric: tabular-nums;
        }
        .events-table tr:nth-child(even) td {
            background: rgba(255,255,255,0.015);
        }
        .events-table tr:hover td {
            background: rgba(6,182,212,0.05);
        }
        @keyframes spin { 100% { transform: rotate(360deg); } }
        .spinning { animation: spin 0.6s linear infinite; }
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
        .btn-flash { background: linear-gradient(135deg, #06b6d4, #6366f1); color: #ffffff; }
        .btn-flash:hover { filter: brightness(1.15); box-shadow: 0 0 12px rgba(6, 182, 212, 0.4); }
        .btn-ymodem { background: linear-gradient(135deg, #0d9488, #10b981); color: #ffffff; }
        .btn-ymodem:hover { filter: brightness(1.15); box-shadow: 0 0 12px rgba(16, 185, 129, 0.4); }
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
            gap: 0.25rem;
            max-height: none;
            overflow: visible;
        }
        .voltage-item {
            background: #0d1424;
            border: 1px solid var(--border);
            padding: 0.25rem 0.35rem;
            border-radius: 4px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 0.72rem;
            overflow: hidden;
            box-sizing: border-box;
        }
        .voltage-name {
            color: var(--text-muted);
            font-size: 0.65rem;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
            max-width: 56px;
        }
        .voltage-num {
            font-family: monospace;
            font-weight: 600;
            color: #38bdf8;
            font-size: 0.72rem;
            white-space: nowrap;
        }
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
            overflow-y: auto;
            position: relative;
            border: 2px solid transparent;
            font-family: 'Lucida Console', 'Courier New', Monaco, monospace;
            transition: border-color 0.2s, box-shadow 0.2s;
            white-space: pre-wrap;
            word-break: break-all;
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
        <div id="endpointsDisabledBanner" style="display:none; background:rgba(245,158,11,0.12); border:1px solid rgba(245,158,11,0.3); border-radius:8px; padding:0.6rem 1rem; margin-bottom:1rem; align-items:center; gap:0.6rem; font-size:0.85rem; color:#fbbf24;">
            <span style="font-size:1.1rem;">&#x26A0;</span>
            <span>REST API endpoints are disabled by default (<code>web.endpoints_enabled = false</code>). Web dashboard controls and polling are restricted. Official MCP tools remain active.</span>
        </div>

        <div id="targetTabs" class="tabs-container">
            <!-- Dynamically populated target tabs -->
        </div>

        <div class="dashboard-grid">
            <!-- Left Column: Controls & Telemetry -->
            <div style="display:flex; flex-direction:column; gap:0.75rem; box-sizing:border-box;">
                <div class="card">
                    <div class="card-header">
                        <span class="card-title">&#x26A1; Power & Target State</span>
                        <span id="powerBadge" class="status-pill" style="background:rgba(244,63,94,0.15); color:var(--accent-rose); border-color:rgba(244,63,94,0.3)">OFF</span>
                    </div>
                    <div class="btn-group">
                        <button class="btn btn-power-on" onclick="controlPower('on')">&#x25B6; Power ON</button>
                        <button class="btn btn-power-off" onclick="requestPowerOff()">&#x25A0; Power OFF</button>
                        <button class="btn btn-reboot" onclick="requestReboot()">&#x21BB; Reboot</button>
                        <button id="btnUsbBoot" class="btn btn-usb-boot" onclick="requestUsbBoot()">&#x1F50C; USB Boot</button>
                        <button class="btn btn-flash" onclick="openFlashModal()">&#x26A1; Flash Firmware</button>
                        <button class="btn btn-ymodem" onclick="openYmodemModal()">&#x1F4E5; YMODEM Load</button>
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

                <div class="card" style="display:flex; flex-direction:column; gap:0.6rem;">
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

        <!-- Board Statistics GUI Card (Carried by Active Board) -->
        <div class="card board-stats-card" id="boardStatsCard">
            <!-- Header Bar: Board Name, Health Badge, Window Selector, Refresh -->
            <div class="stats-header">
                <div style="display:flex; align-items:center; gap:0.75rem; flex-wrap:wrap;">
                    <span class="card-title">&#x1F4CA; <span id="statsBoardName">Target</span> Statistics &amp; Lifecycle Health</span>
                    <span class="status-pill" style="font-size:0.7rem; background:rgba(6,182,212,0.1); color:var(--accent-cyan); border-color:rgba(6,182,212,0.25);">SQLite WAL</span>
                </div>

                <div style="display:flex; align-items:center; gap:0.75rem; flex-wrap:wrap;">
                    <div class="stats-window-group">
                        <button class="window-btn" data-days="1" onclick="setStatsWindow(1)">24h</button>
                        <button class="window-btn" data-days="7" onclick="setStatsWindow(7)">7d</button>
                        <button class="window-btn active" data-days="30" onclick="setStatsWindow(30)">30d</button>
                        <button class="window-btn" data-days="365" onclick="setStatsWindow(365)">1y</button>
                        <button class="window-btn" data-days="0" onclick="setStatsWindow(0)">All</button>
                    </div>
                    <button id="statsRefreshBtn" class="btn btn-secondary" style="padding:0.25rem 0.6rem; font-size:0.75rem;" onclick="refreshStats()" title="Refresh board statistics">&#x21BB; Refresh</button>
                </div>
            </div>

            <!-- Section 1: KPI Summary Metric Cards -->
            <div class="stats-kpi-grid">
                <!-- Flashes KPI -->
                <div class="stat-kpi-card">
                    <div class="stat-kpi-label">&#x26A1; Firmware Flashes</div>
                    <div class="stat-kpi-val" id="kpiFlashesTotal">--</div>
                    <div class="stat-kpi-sub">
                        <span id="kpiFlashRateBadge" class="status-pill" style="font-size:0.7rem;">--%</span>
                        <span id="kpiFlashesLifetime" style="color:var(--text-muted); font-size:0.75rem;">-- lifetime</span>
                    </div>
                </div>

                <!-- Power & Reboots KPI -->
                <div class="stat-kpi-card">
                    <div class="stat-kpi-label">&#x1F50C; Power &amp; Lifecycle</div>
                    <div class="stat-kpi-val" id="kpiPowerCycles">--</div>
                    <div class="stat-kpi-sub">
                        <span id="kpiRebootsTotal" style="color:var(--accent-indigo); font-weight:600;">--</span>
                        <span style="color:var(--text-muted); font-size:0.75rem;">reboots (<span id="kpiRebootsLifetime">--</span> lifetime)</span>
                    </div>
                </div>

                <!-- Intercept & Recovery KPI -->
                <div class="stat-kpi-card">
                    <div class="stat-kpi-label">&#x1F6E1;&#xFE0F; Intercept &amp; Recovery</div>
                    <div class="stat-kpi-val" id="kpiCatches">--</div>
                    <div class="stat-kpi-sub">
                        <span id="kpiUsbDownloads" style="color:var(--accent-amber); font-weight:600;">--</span>
                        <span style="color:var(--text-muted); font-size:0.75rem;">USB recovery boots</span>
                    </div>
                </div>

                <!-- Thermal Profile KPI -->
                <div class="stat-kpi-card">
                    <div class="stat-kpi-label">&#x1F321;&#xFE0F; Thermal &amp; Cooling</div>
                    <div class="stat-kpi-val" id="kpiAvgTemp">-- &deg;C</div>
                    <div class="stat-kpi-sub">
                        <span id="kpiMaxTemp" style="color:var(--accent-rose); font-weight:600;">Peak -- &deg;C</span>
                        <span id="kpiAvgFan" style="color:var(--text-muted); font-size:0.75rem;">Fan -- RPM</span>
                    </div>
                </div>
            </div>

            <!-- Section 2: Wear & Thermal Health Visual Gauges -->
            <div class="stats-visual-grid">
                <!-- eMMC Flash Wear Gauge -->
                <div class="health-card">
                    <div class="health-header">
                        <span style="font-size:0.8rem; font-weight:600;">eMMC Flash Write Stress</span>
                        <span id="flashWearPercent" style="font-size:0.8rem; color:var(--accent-cyan); font-weight:bold;">0.0%</span>
                    </div>
                    <div class="health-bar-track">
                        <div id="flashWearBar" class="health-bar-fill" style="width: 1%;"></div>
                    </div>
                    <div class="health-footer">
                        <span>Low Wear (Nominal endurance: 10,000 cycles)</span>
                        <span id="flashWearCount">-- / 10,000</span>
                    </div>
                </div>

                <!-- Thermal Operating Envelope -->
                <div class="health-card">
                    <div class="health-header">
                        <span style="font-size:0.8rem; font-weight:600;">Thermal Operating Envelope</span>
                        <span id="thermalStatusPill" class="status-pill" style="font-size:0.7rem;">Nominal</span>
                    </div>
                    <div class="temp-envelope-track">
                        <div id="tempEnvelopeBar" class="temp-envelope-bar"></div>
                    </div>
                    <div class="health-footer">
                        <span id="thermalMinMax">Min: -- &deg;C | Max: -- &deg;C</span>
                        <span>Safe Ceiling: 80 &deg;C</span>
                    </div>
                </div>
            </div>

            <!-- Section 3: Recent Lifecycle Events Audit Log -->
            <div class="events-container">
                <div class="events-toolbar">
                    <span style="font-size:0.85rem; font-weight:600;">Recent Lifecycle Events (<span id="eventsTargetLabel">-</span>)</span>
                    <div class="events-filter-group">
                        <button class="filter-chip active" data-filter="all" onclick="filterEvents('all')">All</button>
                        <button class="filter-chip" data-filter="flash" onclick="filterEvents('flash')">Flashes</button>
                        <button class="filter-chip" data-filter="reboot" onclick="filterEvents('reboot')">Reboots</button>
                        <button class="filter-chip" data-filter="catch" onclick="filterEvents('bootloader_catch')">Catches</button>
                    </div>
                </div>
                <div class="events-table-wrapper">
                    <table class="events-table">
                        <thead>
                            <tr>
                                <th style="width:160px;">Timestamp</th>
                                <th style="width:130px;">Event</th>
                                <th style="width:90px;">Status</th>
                                <th style="width:90px;">Duration</th>
                                <th>Context / Details</th>
                            </tr>
                        </thead>
                        <tbody id="eventsTableBody">
                            <tr><td colspan="5" style="text-align:center; color:var(--text-muted); padding:1rem;">Loading events...</td></tr>
                        </tbody>
                    </table>
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
                    <div style="display:flex; gap:0.5rem;">
                        <input id="flashFwInput" class="flash-form-input" style="flex:1;" type="text" placeholder="/path/to/firmware.bin">
                        <button type="button" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.8rem; white-space:nowrap;" onclick="openFileChooser('flashFwInput', '.bin,.pkg')">&#x1F4C1; Browse</button>
                    </div>
                </div>

                <div style="display:grid; grid-template-columns: 1fr 1fr; gap:0.6rem;">
                    <div class="flash-form-group">
                        <label class="flash-form-label">UST Image (Optional)</label>
                        <div style="display:flex; gap:0.4rem;">
                            <input id="flashUstInput" class="flash-form-input" style="flex:1; min-width:0;" type="text" placeholder="/path/to/ust.bin">
                            <button type="button" class="btn btn-secondary" style="padding:0.3rem 0.5rem; font-size:0.75rem; white-space:nowrap;" onclick="openFileChooser('flashUstInput', '.bin')" title="Browse server for UST image">&#x1F4C1;</button>
                        </div>
                    </div>
                    <div class="flash-form-group">
                        <label class="flash-form-label">BLD Image (Optional)</label>
                        <div style="display:flex; gap:0.4rem;">
                            <input id="flashBldInput" class="flash-form-input" style="flex:1; min-width:0;" type="text" placeholder="/path/to/bld.bin">
                            <button type="button" class="btn btn-secondary" style="padding:0.3rem 0.5rem; font-size:0.75rem; white-space:nowrap;" onclick="openFileChooser('flashBldInput', '.bin')" title="Browse server for BLD image">&#x1F4C1;</button>
                        </div>
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

    <!-- YMODEM Download Modal -->
    <div id="ymodemModal" class="modal-backdrop" style="display:none;">
        <div class="modal-card" style="max-width:560px;">
            <div class="modal-title" style="color:var(--accent-emerald);">
                <span>&#x1F4E5;</span> YMODEM / XMODEM Load: <span id="ymodemTargetName" style="color:var(--text-main);">-</span>
            </div>
            <div class="modal-desc" style="display:flex; flex-direction:column; gap:0.8rem;">
                <div class="flash-form-group">
                    <label class="flash-form-label">Firmware / Binary File Path</label>
                    <div style="display:flex; gap:0.5rem;">
                        <input id="ymodemFileInput" class="flash-form-input" style="flex:1;" type="text" placeholder="/path/to/firmware.bin">
                        <button type="button" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.8rem; white-space:nowrap;" onclick="openFileChooser('ymodemFileInput', '.bin,.pkg,.img')">&#x1F4C1; Browse</button>
                    </div>
                </div>

                <div style="display:grid; grid-template-columns: 1fr 1fr; gap:0.6rem;">
                    <div class="flash-form-group">
                        <label class="flash-form-label">Protocol</label>
                        <select id="ymodemProtoSelect" class="flash-form-input" style="height:34px; cursor:pointer;">
                            <option value="ymodem" selected>YMODEM (loady)</option>
                            <option value="xmodem">XMODEM (loadx)</option>
                        </select>
                    </div>
                    <div class="flash-form-group">
                        <label class="flash-form-label">DRAM Load Address</label>
                        <input id="ymodemAddressInput" class="flash-form-input" type="text" value="0x20000000" placeholder="0x20000000">
                    </div>
                </div>

                <div style="display:grid; grid-template-columns: 1fr 1fr; gap:0.6rem;">
                    <div class="flash-form-group">
                        <label class="flash-form-label">U-Boot Auto-Command</label>
                        <label style="display:flex; align-items:center; gap:0.5rem; height:34px; cursor:pointer; font-size:0.85rem; color:var(--text-main);">
                            <input id="ymodemSendCommand" type="checkbox" checked>
                            <span>Send loady / loadx</span>
                        </label>
                    </div>
                    <div class="flash-form-group">
                        <label class="flash-form-label">Timeout (seconds)</label>
                        <input id="ymodemTimeoutInput" class="flash-form-input" type="number" value="300" min="10" max="3600">
                    </div>
                </div>

                <div id="ymodemStatusArea" style="display:none; flex-direction:column; gap:0.4rem;">
                    <div style="display:flex; justify-content:space-between; align-items:center; font-size:0.8rem;">
                        <span id="ymodemStatusMsg" style="font-weight:600; color:var(--accent-cyan);">Transfer in progress...</span>
                        <span id="ymodemTimer" style="color:var(--text-muted); font-size:0.75rem;"></span>
                    </div>
                    <pre id="ymodemLogArea" class="flash-log-box"></pre>
                </div>
            </div>
            <div class="modal-actions">
                <button id="ymodemCancelBtn" class="btn btn-secondary" onclick="closeYmodemModal()">Cancel</button>
                <button id="ymodemSubmitBtn" class="btn btn-ymodem" style="grid-column:unset;" onclick="executeYmodemTransfer()">&#x1F4E5; Load Now</button>
            </div>
        </div>
    </div>

    <!-- Server File Chooser Modal -->
    <div id="fileChooserModal" class="modal-backdrop" style="display:none; z-index:1100;">
        <div class="modal-card" style="max-width:680px; width:90%; max-height:85vh; display:flex; flex-direction:column;">
            <div class="modal-title" style="color:var(--accent-cyan); display:flex; justify-content:space-between; align-items:center;">
                <span>&#x1F4C1; Select File on Server</span>
                <span id="fcExtBadge" class="status-pill" style="font-size:0.7rem; color:var(--text-muted);">*.*</span>
            </div>
            
            <!-- Quick Bookmarks -->
            <div style="display:flex; gap:0.4rem; flex-wrap:wrap; margin-bottom:0.5rem;">
                <span style="font-size:0.75rem; color:var(--text-muted); align-self:center;">Shortcuts:</span>
                <button type="button" class="btn btn-secondary" style="padding:0.15rem 0.45rem; font-size:0.75rem;" onclick="loadDir('~/work/amba-virt/boot/bootstrap/n1-655')">amba-virt bootstrap</button>
                <button type="button" class="btn btn-secondary" style="padding:0.15rem 0.45rem; font-size:0.75rem;" onclick="loadDir('~/work')">~/work</button>
                <button type="button" class="btn btn-secondary" style="padding:0.15rem 0.45rem; font-size:0.75rem;" onclick="loadDir('~')">Home</button>
                <button type="button" class="btn btn-secondary" style="padding:0.15rem 0.45rem; font-size:0.75rem;" onclick="loadDir('/tmp')">/tmp</button>
            </div>

            <!-- Path Bar & Up Button -->
            <div style="display:flex; gap:0.4rem; align-items:center; margin-bottom:0.5rem;">
                <button type="button" id="fcUpBtn" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.8rem;" onclick="navigateUpDir()" title="Go up one level">&#x2B06; Up</button>
                <input id="fcPathInput" class="flash-form-input" style="flex:1; font-family:monospace; font-size:0.8rem;" type="text" onkeydown="if(event.key==='Enter') loadDir(this.value)">
                <button type="button" class="btn btn-secondary" style="padding:0.3rem 0.6rem; font-size:0.8rem;" onclick="loadDir(document.getElementById('fcPathInput').value)">Go</button>
            </div>

            <!-- Filter bar -->
            <div style="margin-bottom:0.5rem;">
                <input id="fcFilterInput" class="flash-form-input" style="width:100%; font-size:0.8rem;" type="text" placeholder="Filter files in directory..." oninput="filterFileList(this.value)">
            </div>

            <!-- File List Container -->
            <div id="fcListContainer" style="flex:1; overflow-y:auto; min-height:220px; max-height:360px; background:#050811; border:1px solid var(--border); border-radius:6px; padding:0.25rem;">
                <div id="fcEntries" style="display:flex; flex-direction:column; gap:2px;"></div>
            </div>

            <!-- Selected File Display -->
            <div style="margin-top:0.6rem; font-size:0.8rem; color:var(--text-muted); display:flex; gap:0.4rem; align-items:center;">
                <span style="font-weight:600;">Selected:</span>
                <span id="fcSelectedPath" style="color:var(--text-main); font-family:monospace; word-break:break-all;">(none)</span>
            </div>

            <!-- Modal Actions -->
            <div class="modal-actions" style="margin-top:0.8rem;">
                <button type="button" class="btn btn-secondary" onclick="closeFileChooser()">Cancel</button>
                <button type="button" id="fcSelectBtn" class="btn btn-flash" style="grid-column:unset;" onclick="confirmFileChooserSelection()" disabled>Choose This File</button>
            </div>
        </div>
    </div>

    <script>
        let targets = [];
        let activeTarget = '';
        let activeTargetData = null;
        let activeConsole = '';
        let ws = null;
        let wsReconnectTimer = null;
        let isFrozen = false;
        let frozenBuffer = [];
        let rawLogLines = [];
        let pendingPowerAction = null;
        let statsWindowDays = 30;
        let currentBoardEvents = [];
        let currentEventFilter = 'all';

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
            const mcu = (activeTargetData && activeTargetData.mcu) || null;
            const ps = (mcu && mcu.powerState) ? mcu.powerState : 'unknown';

            if (ps === 'unknown') {
                alert('Power state unknown — waiting for MCU telemetry. Cannot determine whether to power on or reset.');
                return;
            }

            pendingPowerAction = 'usb_download';
            const btn = document.getElementById('modalConfirmBtn');
            btn.className = 'btn btn-usb-boot';

            if (ps === 'off') {
                document.getElementById('modalTitle').innerHTML = '&#x1F50C; Power On to USB Boot';
                document.getElementById('modalDesc').textContent = `Target "${activeTarget}" is currently powered off. Will power on directly into USB bootloader mode (pwr on -usb).`;
                btn.textContent = 'Power On to USB Boot';
            } else {
                document.getElementById('modalTitle').innerHTML = '&#x1F50C; Reboot to USB Boot';
                document.getElementById('modalDesc').textContent = `Target "${activeTarget}" is currently powered on. The SoC will be rebooted into USB download mode (reset -usb -y).`;
                btn.textContent = 'Reboot to USB Boot';
            }

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

        let isYmodemOperation = false;

        function openYmodemModal() {
            if (!activeTarget) return;
            document.getElementById('ymodemTargetName').textContent = activeTarget;
            document.getElementById('ymodemStatusArea').style.display = 'none';
            document.getElementById('ymodemLogArea').textContent = '';
            document.getElementById('ymodemSubmitBtn').disabled = false;
            document.getElementById('ymodemCancelBtn').disabled = false;
            document.getElementById('ymodemModal').style.display = 'flex';
        }

        function closeYmodemModal() {
            if (isYmodemOperation) {
                if (!confirm("Transfer is currently in progress. Close modal anyway?")) return;
            }
            document.getElementById('ymodemModal').style.display = 'none';
        }

        async function executeYmodemTransfer() {
            if (!activeTarget || isYmodemOperation) return;

            const filePath = document.getElementById('ymodemFileInput').value.trim();
            const protocol = document.getElementById('ymodemProtoSelect').value;
            const address = document.getElementById('ymodemAddressInput').value.trim() || '0x20000000';
            const sendCommand = document.getElementById('ymodemSendCommand').checked;
            const timeout = parseInt(document.getElementById('ymodemTimeoutInput').value, 10) || 300;

            if (!filePath) {
                alert('Please specify the binary file path to transfer.');
                return;
            }

            isYmodemOperation = true;
            const submitBtn = document.getElementById('ymodemSubmitBtn');
            const cancelBtn = document.getElementById('ymodemCancelBtn');
            const statusArea = document.getElementById('ymodemStatusArea');
            const statusMsg = document.getElementById('ymodemStatusMsg');
            const logArea = document.getElementById('ymodemLogArea');

            submitBtn.disabled = true;
            submitBtn.textContent = 'Transferring...';
            cancelBtn.disabled = true;
            statusArea.style.display = 'flex';
            statusMsg.textContent = `Starting ${protocol.toUpperCase()} transfer...`;
            statusMsg.style.color = 'var(--accent-cyan)';
            logArea.textContent = `Initiating ${protocol.toUpperCase()} transfer for ${filePath} to ${activeTarget} (address: ${address})...\n`;

            const startTime = Date.now();
            const timerEl = document.getElementById('ymodemTimer');
            const timerInterval = setInterval(() => {
                const elapsedSec = ((Date.now() - startTime) / 1000).toFixed(1);
                timerEl.textContent = `${elapsedSec}s`;
            }, 200);

            try {
                const res = await fetch(`/api/targets/${activeTarget}/console/load`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        file_path: filePath,
                        protocol: protocol,
                        address: address,
                        send_command: sendCommand,
                        timeout: timeout,
                        console: activeConsole || undefined
                    })
                });

                clearInterval(timerInterval);
                const data = await res.json();

                if (res.ok && data.success) {
                    statusMsg.textContent = `✓ Transfer Succeeded! (${data.bytesSent} bytes in ${data.elapsedSec.toFixed(1)}s at ${data.transferRateKBps.toFixed(1)} KB/s)`;
                    statusMsg.style.color = 'var(--accent-emerald)';
                    logArea.textContent += `[SUCCESS] Protocol: ${data.protocol}\nBytes: ${data.bytesSent} / ${data.fileSize}\nPackets: ${data.packetsSent}, Retries: ${data.retries}\nRate: ${data.transferRateKBps.toFixed(2)} KB/s\n`;
                    if (data.capturedOutput) {
                        logArea.textContent += `\n--- Output ---\n${data.capturedOutput}\n`;
                    }
                } else {
                    const err = data.errorMessage || data.error || ('HTTP ' + res.status);
                    statusMsg.textContent = `✗ Transfer Failed: ${err}`;
                    statusMsg.style.color = 'var(--accent-rose)';
                    logArea.textContent += `\n[ERROR] ${err}\n`;
                    if (data.capturedOutput) {
                        logArea.textContent += `\n--- Output ---\n${data.capturedOutput}\n`;
                    }
                }
            } catch (err) {
                clearInterval(timerInterval);
                statusMsg.textContent = `✗ Transfer Error: ${err.message}`;
                statusMsg.style.color = 'var(--accent-rose)';
                logArea.textContent += `\n[NETWORK ERROR] ${err.message}\n`;
            } finally {
                isYmodemOperation = false;
                submitBtn.disabled = false;
                submitBtn.textContent = '📥 Load Now';
                cancelBtn.disabled = false;
            }
        }

        // Server File Chooser Logic
        let fcTargetInputId = '';
        let fcCurrentExt = '';
        let fcCurrentDir = '';
        let fcParentDir = '';
        let fcRawEntries = [];
        let fcSelectedFile = '';

        function openFileChooser(targetInputId, extFilter) {
            fcTargetInputId = targetInputId;
            fcCurrentExt = extFilter || '';
            fcSelectedFile = '';
            document.getElementById('fcExtBadge').textContent = extFilter ? extFilter : '*.*';
            document.getElementById('fcSelectedPath').textContent = '(none)';
            document.getElementById('fcSelectBtn').disabled = true;
            document.getElementById('fcFilterInput').value = '';

            let initialPath = '';
            const targetEl = document.getElementById(targetInputId);
            if (targetEl && targetEl.value.trim()) {
                initialPath = targetEl.value.trim();
            }
            loadDir(initialPath);
            document.getElementById('fileChooserModal').style.display = 'flex';
        }

        function closeFileChooser() {
            document.getElementById('fileChooserModal').style.display = 'none';
        }

        async function loadDir(path) {
            try {
                let url = '/api/fs/browse?';
                if (path) url += 'path=' + encodeURIComponent(path) + '&';
                if (fcCurrentExt) url += 'ext=' + encodeURIComponent(fcCurrentExt);
                
                const res = await fetch(url);
                if (!res.ok) {
                    const err = await res.json();
                    alert(err.error || 'Failed to read directory');
                    return;
                }
                const data = await res.json();
                fcCurrentDir = data.current;
                fcParentDir = data.parent || '';
                fcRawEntries = data.entries || [];
                
                document.getElementById('fcPathInput').value = fcCurrentDir;
                document.getElementById('fcUpBtn').disabled = !fcParentDir;
                renderFileList(fcRawEntries);
            } catch (e) {
                console.error('File chooser error:', e);
            }
        }

        function navigateUpDir() {
            if (fcParentDir) {
                loadDir(fcParentDir);
            }
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
        }

        function renderFileList(entries) {
            const container = document.getElementById('fcEntries');
            container.innerHTML = '';
            if (!entries || entries.length === 0) {
                container.innerHTML = '<div style="padding:1rem; text-align:center; color:var(--text-muted); font-size:0.8rem;">No matching files or directories found.</div>';
                return;
            }
            entries.forEach(entry => {
                const item = document.createElement('div');
                item.style.cssText = 'display:flex; justify-content:space-between; align-items:center; padding:0.35rem 0.6rem; border-radius:4px; cursor:pointer; font-size:0.82rem; user-select:none; transition:background 0.15s;';
                item.className = 'fc-item';
                
                const icon = entry.is_dir ? '&#x1F4C1;' : '&#x1F4C4;';
                const sizeStr = entry.is_dir ? '&lt;DIR&gt;' : formatBytes(entry.size);
                
                item.innerHTML = `
                    <div style="display:flex; align-items:center; gap:0.5rem; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;">
                        <span style="font-size:1rem;">${icon}</span>
                        <span style="color:${entry.is_dir ? 'var(--accent-cyan)' : 'var(--text-main)'}; font-weight:${entry.is_dir ? '600' : 'normal'}; font-family:monospace;">${escapeHtml(entry.name)}</span>
                    </div>
                    <span style="color:var(--text-muted); font-size:0.75rem; font-family:monospace; margin-left:1rem; flex-shrink:0;">${sizeStr}</span>
                `;

                item.addEventListener('mouseenter', () => item.style.background = 'rgba(6,182,212,0.12)');
                item.addEventListener('mouseleave', () => {
                    if (fcSelectedFile !== entry.path) item.style.background = 'transparent';
                    else item.style.background = 'rgba(6,182,212,0.25)';
                });

                item.addEventListener('click', () => {
                    if (entry.is_dir) {
                        loadDir(entry.path);
                    } else {
                        document.querySelectorAll('.fc-item').forEach(el => el.style.background = 'transparent');
                        item.style.background = 'rgba(6,182,212,0.25)';
                        fcSelectedFile = entry.path;
                        document.getElementById('fcSelectedPath').textContent = entry.path;
                        document.getElementById('fcSelectBtn').disabled = false;
                    }
                });

                if (!entry.is_dir) {
                    item.addEventListener('dblclick', () => {
                        fcSelectedFile = entry.path;
                        confirmFileChooserSelection();
                    });
                }

                container.appendChild(item);
            });
        }

        function filterFileList(q) {
            const query = q.toLowerCase();
            const filtered = fcRawEntries.filter(e => e.name.toLowerCase().includes(query));
            renderFileList(filtered);
        }

        function confirmFileChooserSelection() {
            if (fcSelectedFile && fcTargetInputId) {
                const targetEl = document.getElementById(fcTargetInputId);
                if (targetEl) {
                    targetEl.value = fcSelectedFile;
                }
                closeFileChooser();
            }
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
                    // Filter automated terminal emulator responses (CPR \x1b[<row>;<col>R, DA \x1b[?<attrs>c, etc.)
                    if (/^\x1b\[\??[0-9;]*[Rcnty]$/.test(data) || /^\x1b\[>[0-9;]*c$/.test(data)) {
                        return;
                    }
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
                if (!res.ok) {
                    if (res.status === 403) {
                        const banner = document.getElementById('endpointsDisabledBanner');
                        if (banner) banner.style.display = 'flex';
                    }
                    return;
                }
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
            const bName = document.getElementById('statsBoardName');
            if (bName) bName.textContent = name;
            const eLabel = document.getElementById('eventsTargetLabel');
            if (eLabel) eLabel.textContent = name;
            pollActiveTarget().then(() => {
                connectWebSocket();
            });
            loadBoardStats(name);
            loadBoardEvents(name);
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
                const isConn = (c.connected !== false);
                const discLabel = isConn ? '' : ' [disconnected]';
                const styleAttr = isConn ? '' : ' style="color: var(--accent-rose);"';
                optionsHtml += `<option value="${c.name}"${styleAttr}>${c.name} (${c.port} : ${c.telnetPort})${discLabel}</option>`;
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
                activeTargetData = data;

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

                const mcuElem = document.getElementById('targetMcu');
                if (data.mcu) {
                    const isConn = data.mcu.connected;
                    const dotColor = isConn ? 'var(--accent-emerald)' : 'var(--accent-rose)';
                    const dot = `<span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${dotColor};margin-right:6px;vertical-align:middle;"></span>`;
                    const label = isConn ? '' : ' <span style="color:var(--accent-rose);font-size:11px;">(disconnected)</span>';
                    mcuElem.innerHTML = `${dot}${data.mcu.port} (telnet ${data.mcu.telnetPort})${label}`;
                } else {
                    mcuElem.textContent = '-';
                }

                const powerBadge = document.getElementById('powerBadge');
                const ps = (data.mcu && data.mcu.powerState) ? data.mcu.powerState : (data.mcu && data.mcu.powerOn ? 'on' : (data.mcu ? 'off' : 'unknown'));
                if (ps === 'on') {
                    powerBadge.textContent = 'POWER ON';
                    powerBadge.style.background = 'rgba(16,185,129,0.15)';
                    powerBadge.style.color = 'var(--accent-emerald)';
                    powerBadge.style.borderColor = 'rgba(16,185,129,0.3)';
                } else if (ps === 'off') {
                    powerBadge.textContent = 'POWER OFF';
                    powerBadge.style.background = 'rgba(244,63,94,0.15)';
                    powerBadge.style.color = 'var(--accent-rose)';
                    powerBadge.style.borderColor = 'rgba(244,63,94,0.3)';
                } else {
                    powerBadge.textContent = 'POWER UNKNOWN';
                    powerBadge.style.background = 'rgba(245,158,11,0.15)';
                    powerBadge.style.color = '#f59e0b';
                    powerBadge.style.borderColor = 'rgba(245,158,11,0.3)';
                }

                const usbBootBtn = document.getElementById('btnUsbBoot');
                if (usbBootBtn) {
                    if (ps === 'unknown') {
                        usbBootBtn.disabled = true;
                        usbBootBtn.title = 'Power state unknown — waiting for MCU telemetry...';
                        usbBootBtn.style.opacity = '0.5';
                        usbBootBtn.style.cursor = 'not-allowed';
                    } else {
                        usbBootBtn.disabled = false;
                        usbBootBtn.title = (ps === 'off') ? 'Power on into USB bootloader mode' : 'Reboot into USB download mode';
                        usbBootBtn.style.opacity = '1.0';
                        usbBootBtn.style.cursor = 'pointer';
                    }
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

        function setStatsWindow(days) {
            statsWindowDays = days;
            document.querySelectorAll('.window-btn').forEach(btn => {
                btn.classList.toggle('active', parseInt(btn.dataset.days) === days);
            });
            if (activeTarget) {
                loadBoardStats(activeTarget);
            }
        }

        async function loadBoardStats(targetName) {
            if (!targetName) return;
            try {
                const res = await fetch(`/api/targets/${encodeURIComponent(targetName)}/stats?window=${statsWindowDays}`);
                if (!res.ok) return;
                const data = await res.json();
                const summary = data.summary || {};
                const lifetime = data.lifetime_totals || {};

                // 1. Firmware flashes
                const flashesTotal = summary.flashes_total || 0;
                const flashesLifetime = lifetime.flashes_total || 0;
                const successRate = summary.flash_success_rate !== undefined ? (summary.flash_success_rate * 100) : 100.0;

                const fTotalEl = document.getElementById('kpiFlashesTotal');
                if (fTotalEl) fTotalEl.textContent = flashesTotal;
                const fLifeEl = document.getElementById('kpiFlashesLifetime');
                if (fLifeEl) fLifeEl.textContent = `${flashesLifetime} lifetime`;
                const rateBadge = document.getElementById('kpiFlashRateBadge');
                if (rateBadge) {
                    rateBadge.textContent = `${successRate.toFixed(1)}%`;
                    if (successRate >= 90) {
                        rateBadge.style.background = 'rgba(16,185,129,0.15)';
                        rateBadge.style.color = 'var(--accent-emerald)';
                        rateBadge.style.borderColor = 'rgba(16,185,129,0.3)';
                    } else if (successRate >= 70) {
                        rateBadge.style.background = 'rgba(245,158,11,0.15)';
                        rateBadge.style.color = 'var(--accent-amber)';
                        rateBadge.style.borderColor = 'rgba(245,158,11,0.3)';
                    } else {
                        rateBadge.style.background = 'rgba(244,63,94,0.15)';
                        rateBadge.style.color = 'var(--accent-rose)';
                        rateBadge.style.borderColor = 'rgba(244,63,94,0.3)';
                    }
                }

                // 2. Power & Reboots
                const pCycles = summary.power_cycles_total || 0;
                const reboots = summary.reboots_total || 0;
                const rebootsLifetime = lifetime.reboots_total || 0;
                const pCyclesEl = document.getElementById('kpiPowerCycles');
                if (pCyclesEl) pCyclesEl.textContent = pCycles;
                const rTotalEl = document.getElementById('kpiRebootsTotal');
                if (rTotalEl) rTotalEl.textContent = reboots;
                const rLifeEl = document.getElementById('kpiRebootsLifetime');
                if (rLifeEl) rLifeEl.textContent = rebootsLifetime;

                // 3. Intercept & Recovery
                const catches = summary.bootloader_catches_total || 0;
                const usbDown = summary.usb_downloads_total || 0;
                const catchesEl = document.getElementById('kpiCatches');
                if (catchesEl) catchesEl.textContent = catches;
                const usbEl = document.getElementById('kpiUsbDownloads');
                if (usbEl) usbEl.textContent = usbDown;

                // 4. Thermal & Fan
                const avgTemp = summary.avg_temp_c || 0.0;
                const maxTemp = summary.max_temp_c || 0.0;
                const minTemp = summary.min_temp_c || 0.0;
                const avgFan = summary.avg_fan_rpm || 0.0;

                const avgTempEl = document.getElementById('kpiAvgTemp');
                if (avgTempEl) avgTempEl.innerHTML = (avgTemp > 0 ? avgTemp.toFixed(1) : '--') + ' &deg;C';
                const maxTempEl = document.getElementById('kpiMaxTemp');
                if (maxTempEl) maxTempEl.innerHTML = (maxTemp > 0 ? `Peak ${maxTemp.toFixed(1)} &deg;C` : 'Peak -- &deg;C');
                const avgFanEl = document.getElementById('kpiAvgFan');
                if (avgFanEl) avgFanEl.textContent = (avgFan > 0 ? `Fan ${Math.round(avgFan)} RPM` : 'Fan -- RPM');

                // 5. eMMC Flash Wear Gauge (Nominal 10,000 P/E cycles)
                const nominalCycles = 10000;
                const wearPercent = Math.min(100, Math.max(0.01, (flashesLifetime / nominalCycles) * 100));
                const wearPercentEl = document.getElementById('flashWearPercent');
                if (wearPercentEl) wearPercentEl.textContent = `${wearPercent.toFixed(2)}%`;
                const wearBar = document.getElementById('flashWearBar');
                if (wearBar) wearBar.style.width = `${Math.max(1, wearPercent)}%`;
                const wearCountEl = document.getElementById('flashWearCount');
                if (wearCountEl) wearCountEl.textContent = `${flashesLifetime} / ${nominalCycles.toLocaleString()}`;

                // 6. Thermal Operating Envelope
                const minMaxText = (minTemp > 0 && maxTemp > 0) ? `Min: ${minTemp.toFixed(1)}°C | Max: ${maxTemp.toFixed(1)}°C` : 'Min: -- °C | Max: -- °C';
                const thermalMinMaxEl = document.getElementById('thermalMinMax');
                if (thermalMinMaxEl) thermalMinMaxEl.textContent = minMaxText;
                const thermalPill = document.getElementById('thermalStatusPill');
                if (thermalPill) {
                    if (maxTemp > 75) {
                        thermalPill.textContent = 'High Temp';
                        thermalPill.style.background = 'rgba(244,63,94,0.15)';
                        thermalPill.style.color = 'var(--accent-rose)';
                        thermalPill.style.borderColor = 'rgba(244,63,94,0.3)';
                    } else if (maxTemp > 60) {
                        thermalPill.textContent = 'Warm';
                        thermalPill.style.background = 'rgba(245,158,11,0.15)';
                        thermalPill.style.color = 'var(--accent-amber)';
                        thermalPill.style.borderColor = 'rgba(245,158,11,0.3)';
                    } else {
                        thermalPill.textContent = 'Nominal';
                        thermalPill.style.background = 'rgba(16,185,129,0.15)';
                        thermalPill.style.color = 'var(--accent-emerald)';
                        thermalPill.style.borderColor = 'rgba(16,185,129,0.3)';
                    }
                }

                // Envelope marker position: 0°C (0%) to 100°C (100%)
                const clampTemp = Math.min(100, Math.max(0, avgTemp > 0 ? avgTemp : 25));
                const envBar = document.getElementById('tempEnvelopeBar');
                if (envBar) envBar.style.left = `calc(${clampTemp}% - 4px)`;
            } catch (err) {
                console.error('Failed to load board stats', err);
            }
        }

        async function loadBoardEvents(targetName) {
            if (!targetName) return;
            try {
                const res = await fetch(`/api/targets/${encodeURIComponent(targetName)}/events?limit=30`);
                if (!res.ok) return;
                currentBoardEvents = await res.json();
                renderBoardEvents();
            } catch (err) {
                console.error('Failed to load board events', err);
            }
        }

        function filterEvents(type) {
            currentEventFilter = type;
            document.querySelectorAll('.filter-chip').forEach(btn => {
                btn.classList.toggle('active', btn.dataset.filter === type);
            });
            renderBoardEvents();
        }

        function renderBoardEvents() {
            const tbody = document.getElementById('eventsTableBody');
            if (!tbody) return;

            let filtered = currentBoardEvents || [];
            if (currentEventFilter === 'flash') {
                filtered = filtered.filter(e => e.event_type === 'flash');
            } else if (currentEventFilter === 'reboot') {
                filtered = filtered.filter(e => e.event_type === 'reboot' || e.event_type === 'power_on' || e.event_type === 'power_off');
            } else if (currentEventFilter === 'bootloader_catch') {
                filtered = filtered.filter(e => e.event_type === 'bootloader_catch');
            }

            if (filtered.length === 0) {
                tbody.innerHTML = '<tr><td colspan="5" style="text-align:center; color:var(--text-muted); padding:1rem;">No events recorded in this category.</td></tr>';
                return;
            }

            let html = '';
            filtered.forEach(ev => {
                const dateStr = new Date(ev.timestamp * 1000).toISOString().replace('T', ' ').substring(0, 19);
                const isSuccess = ev.status === 'success';
                const statusBadge = isSuccess
                    ? '<span class="status-pill" style="font-size:0.68rem; padding:0.1rem 0.45rem; background:rgba(16,185,129,0.15); color:var(--accent-emerald); border-color:rgba(16,185,129,0.3)">SUCCESS</span>'
                    : `<span class="status-pill" style="font-size:0.68rem; padding:0.1rem 0.45rem; background:rgba(244,63,94,0.15); color:var(--accent-rose); border-color:rgba(244,63,94,0.3)">${(ev.status || 'FAILED').toUpperCase()}</span>`;
                const durationStr = ev.duration_ms > 0 ? (ev.duration_ms > 1000 ? (ev.duration_ms / 1000).toFixed(1) + 's' : ev.duration_ms + 'ms') : '-';
                const safeDetails = (ev.details || '-').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

                let typeIcon = '&#x25C6;';
                if (ev.event_type === 'flash') typeIcon = '&#x26A1;';
                else if (ev.event_type === 'reboot') typeIcon = '&#x21BB;';
                else if (ev.event_type === 'power_on' || ev.event_type === 'power_off') typeIcon = '&#x1F50C;';
                else if (ev.event_type === 'bootloader_catch') typeIcon = '&#x1F6E1;&#xFE0F;';
                else if (ev.event_type === 'usb_download') typeIcon = '&#x1F4BE;';

                html += `<tr>
                    <td style="font-family:monospace; color:var(--text-muted);">${dateStr}</td>
                    <td><span style="margin-right:0.3rem;">${typeIcon}</span><b>${ev.event_type}</b></td>
                    <td>${statusBadge}</td>
                    <td style="font-family:monospace;">${durationStr}</td>
                    <td style="font-family:monospace; color:var(--text-muted); word-break:break-all;">${safeDetails}</td>
                </tr>`;
            });
            tbody.innerHTML = html;
        }

        async function refreshStats() {
            if (!activeTarget) return;
            const btn = document.getElementById('statsRefreshBtn');
            if (btn) btn.classList.add('spinning');
            await Promise.all([loadBoardStats(activeTarget), loadBoardEvents(activeTarget)]);
            if (btn) btn.classList.remove('spinning');
        }

        // Initialize components on load
        initTerminal();
        fetchTargets();
        setInterval(pollActiveTarget, 2000);
        setInterval(() => {
            if (activeTarget) {
                loadBoardStats(activeTarget);
                loadBoardEvents(activeTarget);
            }
        }, 15000);
    </script>
</body>
</html>
)rawhtml";
    return html;
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
