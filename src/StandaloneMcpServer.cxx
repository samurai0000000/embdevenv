/*
 * StandaloneMcpServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "StandaloneMcpServer.hxx"
#include "ToolDispatcher.hxx"
#include "Version.hxx"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace embdevenv {

using json = nlohmann::json;

StandaloneMcpSession::StandaloneMcpSession(int clientFd, struct sockaddr_in addr)
    : _clientFd(clientFd)
    , _addr(addr)
    , _running(false)
{
}

StandaloneMcpSession::~StandaloneMcpSession()
{
    stop();
}

bool StandaloneMcpSession::start()
{
    if (_running) {
        return true;
    }

    _running = true;
    _workerThread = std::thread(&StandaloneMcpSession::sessionWorker, this);
    return true;
}

void StandaloneMcpSession::stop()
{
    if (!_running) {
        return;
    }

    _running = false;
    if (_clientFd >= 0) {
        ::shutdown(_clientFd, SHUT_RDWR);
        ::close(_clientFd);
        _clientFd = -1;
    }

    if (_workerThread.joinable()) {
        _workerThread.join();
    }
}

bool StandaloneMcpSession::isRunning() const
{
    return _running;
}

void StandaloneMcpSession::sendResponse(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(_writeMutex);
    if (_clientFd < 0 || msg.empty()) {
        return;
    }

    std::string payload = msg;
    if (payload.back() != '\n') {
        payload += '\n';
    }

    const char *ptr = payload.c_str();
    size_t rem = payload.size();
    while (rem > 0 && _running) {
        ssize_t n = ::send(_clientFd, ptr, rem, MSG_NOSIGNAL);
        if (n > 0) {
            ptr += static_cast<size_t>(n);
            rem -= static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = _clientFd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 100) <= 0) {
                break;
            }
        } else {
            _running = false;
            break;
        }
    }
}

void StandaloneMcpSession::handleLine(const std::string &line)
{
    try {
        json req = json::parse(line);

        // Notifications don't have id and do not expect a response
        if (!req.contains("id")) {
            return;
        }

        auto reqId = req["id"];
        std::string method = req.value("method", "");

        if (method == "initialize") {
            json resp = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", {
                    {"protocolVersion", "2024-11-05"},
                    {"capabilities", {
                        {"tools", json::object()}
                    }},
                    {"serverInfo", {
                        {"name", "embdevenv"},
                        {"version", EMBDEVENV_VERSION_STRING}
                    }}
                }}
            };
            sendResponse(resp.dump());
        } else if (method == "tools/list") {
            json resp = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", {
                    {"tools", ToolDispatcher::getToolDefinitions()}
                }}
            };
            sendResponse(resp.dump());
        } else if (method == "tools/call") {
            json params = req.value("params", json::object());
            std::string toolName = params.value("name", "");
            json args = params.value("arguments", json::object());

            std::string resultText = ToolDispatcher::dispatchTool(toolName, args);

            json resp = {
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
            sendResponse(resp.dump());
        } else if (method == "ping") {
            json resp = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", json::object()}
            };
            sendResponse(resp.dump());
        } else {
            json errResp = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"error", {
                    {"code", -32601},
                    {"message", "Method not found: " + method}
                }}
            };
            sendResponse(errResp.dump());
        }
    } catch (const std::exception &ex) {
        fprintf(stderr, "[embdevenv] StandaloneMcpSession parse error: %s\n", ex.what());
    }
}

void StandaloneMcpSession::sessionWorker()
{
    char buf[4096];
    std::string lineBuffer;

    while (_running) {
        struct pollfd pfd;
        pfd.fd = _clientFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 300);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::recv(_clientFd, buf, sizeof(buf), 0);
            if (n > 0) {
                lineBuffer.append(buf, static_cast<size_t>(n));
                size_t nlPos;
                while ((nlPos = lineBuffer.find('\n')) != std::string::npos) {
                    std::string line = lineBuffer.substr(0, nlPos);
                    lineBuffer.erase(0, nlPos + 1);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (!line.empty()) {
                        handleLine(line);
                    }
                }
            } else {
                break;
            }
        } else if (ret > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            break;
        } else if (ret < 0 && errno != EINTR) {
            break;
        }
    }

    _running = false;
}

StandaloneMcpServer::StandaloneMcpServer(int port)
    : _port(port)
    , _listenFd(-1)
    , _running(false)
{
}

StandaloneMcpServer::~StandaloneMcpServer()
{
    stop();
}

bool StandaloneMcpServer::start()
{
    if (_running) {
        return true;
    }

    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0) {
        fprintf(stderr, "[embdevenv] StandaloneMcpServer: socket creation failed: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(_listenFd, F_GETFL, 0);
    fcntl(_listenFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(_port);

    if (::bind(_listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[embdevenv] StandaloneMcpServer: bind failed on port %d: %s\n",
                _port, strerror(errno));
        ::close(_listenFd);
        _listenFd = -1;
        return false;
    }

    if (::listen(_listenFd, 5) < 0) {
        fprintf(stderr, "[embdevenv] StandaloneMcpServer: listen failed: %s\n", strerror(errno));
        ::close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _running = true;
    _acceptThread = std::thread(&StandaloneMcpServer::acceptWorker, this);
    fprintf(stderr, "[embdevenv] StandaloneMcpServer listening on port %d\n", _port);
    return true;
}

void StandaloneMcpServer::stop()
{
    if (!_running) {
        return;
    }

    _running = false;

    if (_listenFd >= 0) {
        ::close(_listenFd);
        _listenFd = -1;
    }

    if (_acceptThread.joinable()) {
        _acceptThread.join();
    }

    std::lock_guard<std::mutex> lock(_sessionsMutex);
    for (auto &s : _sessions) {
        s->stop();
    }
    _sessions.clear();
    fprintf(stderr, "[embdevenv] StandaloneMcpServer stopped\n");
}

bool StandaloneMcpServer::isRunning() const
{
    return _running;
}

int StandaloneMcpServer::getPort() const
{
    return _port;
}

void StandaloneMcpServer::pruneSessions()
{
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    _sessions.erase(
        std::remove_if(_sessions.begin(), _sessions.end(),
                       [](const std::shared_ptr<StandaloneMcpSession> &s) {
                           return !s->isRunning();
                       }),
        _sessions.end());
}

void StandaloneMcpServer::acceptWorker()
{
    while (_running) {
        struct pollfd pfd;
        pfd.fd = _listenFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = ::accept(_listenFd, reinterpret_cast<struct sockaddr *>(&clientAddr),
                                    &clientLen);
            if (clientFd >= 0) {
                auto session = std::make_shared<StandaloneMcpSession>(clientFd, clientAddr);
                if (session->start()) {
                    std::lock_guard<std::mutex> lock(_sessionsMutex);
                    _sessions.push_back(session);
                }
            }
        }

        pruneSessions();
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
