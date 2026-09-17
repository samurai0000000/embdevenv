/*
 * AimonGatewayClient.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AimonGatewayClient.hxx"
#include "ToolDispatcher.hxx"
#include "Config.hxx"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

namespace embdevenv {

using json = nlohmann::json;

AimonGatewayClient &AimonGatewayClient::getInstance()
{
    static AimonGatewayClient instance;
    return instance;
}

AimonGatewayClient::AimonGatewayClient()
    : _host("127.0.0.1")
    , _port(3885)
    , _running(false)
    , _connected(false)
    , _sockFd(-1)
{
}

AimonGatewayClient::~AimonGatewayClient()
{
    stop();
    join();
}

bool AimonGatewayClient::start(const std::string &host, int port)
{
    if (_running.load()) {
        return true;
    }

    if (!host.empty()) {
        _host = host;
    } else {
        _host = Config::getInstance().getMcpConfig().aimonHost;
    }

    if (port != 0) {
        _port = port;
    } else {
        _port = Config::getInstance().getMcpConfig().aimonPort;
    }

    _running.store(true);
    _thread = std::thread(&AimonGatewayClient::run, this);
    return true;
}

void AimonGatewayClient::stop()
{
    if (!_running.load()) {
        return;
    }

    _running.store(false);
    disconnect();
}

void AimonGatewayClient::join()
{
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool AimonGatewayClient::isConnected() const
{
    return _connected.load();
}

void AimonGatewayClient::disconnect()
{
    _connected.store(false);
    std::lock_guard<std::mutex> lock(_sendMutex);
    if (_sockFd != -1) {
        shutdown(_sockFd, SHUT_RDWR);
        close(_sockFd);
        _sockFd = -1;
    }
}

bool AimonGatewayClient::connectToGateway()
{
    disconnect();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    std::string portStr = std::to_string(_port);
    int rc = getaddrinfo(_host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        return false;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }

        int flag = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char *>(&flag), sizeof(flag));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd == -1) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_sendMutex);
        _sockFd = fd;
    }

    return true;
}

void AimonGatewayClient::sendResponse(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(_sendMutex);
    if (_sockFd == -1) {
        return;
    }

    std::string payload = msg;
    if (payload.empty() || payload.back() != '\n') {
        payload += "\n";
    }

    const char *ptr = payload.c_str();
    size_t rem = payload.size();
    while (rem > 0) {
        ssize_t n = send(_sockFd, ptr, rem, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        ptr += static_cast<size_t>(n);
        rem -= static_cast<size_t>(n);
    }
}

bool AimonGatewayClient::sendRegistration()
{
    json reg = {
        {"jsonrpc", "2.0"},
        {"method", "gateway/register"},
        {"params", {
            {"subsystem", "embdevenv"},
            {"name", "embdevenv"},
            {"description", "Embedded hardware development environment, target console demuxer, and MCU telemetry gateway"},
            {"tools", ToolDispatcher::getToolDefinitions()}
        }}
    };

    sendResponse(reg.dump());
    return true;
}

void AimonGatewayClient::processIncoming()
{
    char buffer[4096];
    std::string lineBuffer;

    while (_running.load() && _connected.load() && (_sockFd != -1)) {
        struct pollfd pfd;
        pfd.fd = _sockFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = recv(_sockFd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            lineBuffer.append(buffer, static_cast<size_t>(n));

            size_t newlinePos;
            while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, newlinePos);
                lineBuffer.erase(0, newlinePos + 1);
                if (!line.empty() && (line.back() == '\r')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    handleRequest(line);
                }
            }
        }
    }
}

void AimonGatewayClient::handleRequest(const std::string &line)
{
    try {
        json req = json::parse(line);
        if (!req.contains("id")) {
            return;
        }
        if (req.contains("result") || req.contains("error")) {
            return;
        }

        auto reqId = req["id"];
        std::string method = req.value("method", "");

        if (method == "tools/call") {
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
        } else if (method == "tools/list") {
            json resp = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", {
                    {"tools", ToolDispatcher::getToolDefinitions()}
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
        fprintf(stderr, "[embdevenv] AimonGatewayClient: request parse error: %s\n", ex.what());
    }
}

void AimonGatewayClient::run()
{
    int reconnectInterval = Config::getInstance().getMcpConfig().reconnectIntervalSec;
    if (reconnectInterval <= 0) {
        reconnectInterval = 5;
    }

    fprintf(stderr, "[embdevenv] AimonGatewayClient connecting to %s:%d\n", _host.c_str(), _port);

    while (_running.load()) {
        if (connectToGateway()) {
            _connected.store(true);
            fprintf(stderr, "[embdevenv] Connected to aimon gateway at %s:%d\n", _host.c_str(), _port);

            if (sendRegistration()) {
                processIncoming();
            }

            _connected.store(false);
            disconnect();
            fprintf(stderr, "[embdevenv] Disconnected from aimon gateway\n");
        }

        if (!_running.load()) {
            break;
        }

        for (int i = 0; i < reconnectInterval && _running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
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
