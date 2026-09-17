/*
 * TelnetServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TelnetServer.hxx"
#include "Console.hxx"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace embdevenv {

// RFC 854 constants
static constexpr unsigned char IAC  = 255;
static constexpr unsigned char DONT = 254;
static constexpr unsigned char DO   = 253;
static constexpr unsigned char WONT = 252;
static constexpr unsigned char WILL = 251;
static constexpr unsigned char SB   = 250;
static constexpr unsigned char SE   = 240;

TelnetSession::TelnetSession(int clientFd, struct sockaddr_in addr, Console *console)
    : _clientFd(clientFd)
    , _addr(addr)
    , _console(console)
    , _running(false)
{
}

TelnetSession::~TelnetSession()
{
    stop();
}

bool TelnetSession::start()
{
    if (_running) {
        return true;
    }

    _running = true;

    // Send Telnet character-mode negotiation
    const unsigned char initTelnet[] = {
        IAC, WILL, 1, // IAC WILL ECHO
        IAC, WILL, 3, // IAC WILL SUPPRESS GO AHEAD
        IAC, DO,   0, // IAC DO BINARY
        IAC, WILL, 0  // IAC WILL BINARY
    };
    sendData(reinterpret_cast<const char *>(initTelnet), sizeof(initTelnet));

    _workerThread = std::thread(&TelnetSession::sessionWorker, this);
    return true;
}

void TelnetSession::stop()
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

bool TelnetSession::isRunning() const
{
    return _running;
}

void TelnetSession::sendData(const char *data, size_t len)
{
    if (!_running || _clientFd < 0 || data == nullptr || len == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(_writeMutex);
    size_t total = 0;
    while (total < len && _running) {
        ssize_t n = ::send(_clientFd, data + total, len - total, MSG_NOSIGNAL);
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = _clientFd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int ret = poll(&pfd, 1, 100);
            if (ret <= 0) {
                break;
            }
        } else {
            _running = false;
            break;
        }
    }
}

std::string TelnetSession::getClientIp() const
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(_addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip);
}

int TelnetSession::getClientPort() const
{
    return ntohs(_addr.sin_port);
}

void TelnetSession::processIncomingBytes(const char *buf, size_t len)
{
    if (_console == nullptr || buf == nullptr || len == 0) {
        return;
    }

    std::string cleanData;
    cleanData.reserve(len);

    size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == IAC) {
            if (i + 1 >= len) {
                break; // Incomplete IAC sequence
            }
            unsigned char cmd = static_cast<unsigned char>(buf[i + 1]);
            if (cmd == IAC) {
                // Escaped IAC byte
                cleanData += static_cast<char>(IAC);
                i += 2;
            } else if (cmd == WILL || cmd == WONT || cmd == DO || cmd == DONT) {
                // 3-byte command
                i += 3;
            } else if (cmd == SB) {
                // Subnegotiation until SE
                i += 2;
                while (i + 1 < len) {
                    if (static_cast<unsigned char>(buf[i]) == IAC &&
                        static_cast<unsigned char>(buf[i + 1]) == SE) {
                        i += 2;
                        break;
                    }
                    ++i;
                }
            } else {
                // 2-byte simple command
                i += 2;
            }
        } else if (c == '\0') {
            // Telnet CR-NUL or null byte filtering
            ++i;
        } else {
            cleanData += static_cast<char>(c);
            ++i;
        }
    }

    if (!cleanData.empty()) {
        _console->write(cleanData);
    }
}

void TelnetSession::sessionWorker()
{
    char buf[512];

    while (_running) {
        struct pollfd pfd;
        pfd.fd = _clientFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::recv(_clientFd, buf, sizeof(buf), 0);
            if (n > 0) {
                processIncomingBytes(buf, static_cast<size_t>(n));
            } else {
                // Connection closed or error
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

TelnetServer::TelnetServer(int port, Console *console)
    : _port(port)
    , _console(console)
    , _listenFd(-1)
    , _consoleListenerId(-1)
    , _running(false)
{
}

TelnetServer::~TelnetServer()
{
    stop();
}

bool TelnetServer::start()
{
    if (_running) {
        return true;
    }

    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0) {
        fprintf(stderr, "[embdevenv] TelnetServer: failed to create socket: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Make non-blocking
    int flags = fcntl(_listenFd, F_GETFL, 0);
    fcntl(_listenFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(_port);

    if (::bind(_listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[embdevenv] TelnetServer: failed to bind port %d: %s\n",
                _port, strerror(errno));
        ::close(_listenFd);
        _listenFd = -1;
        return false;
    }

    if (::listen(_listenFd, 5) < 0) {
        fprintf(stderr, "[embdevenv] TelnetServer: failed to listen on port %d: %s\n",
                _port, strerror(errno));
        ::close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _running = true;

    // Register callback with Console to receive output
    if (_console != nullptr) {
        _consoleListenerId = _console->addListener(
            [this](const char *data, size_t len) {
                broadcastConsoleOutput(data, len);
            });
    }

    _acceptThread = std::thread(&TelnetServer::acceptWorker, this);
    fprintf(stderr, "[embdevenv] TelnetServer listening on port %d\n", _port);
    return true;
}

void TelnetServer::stop()
{
    if (!_running) {
        return;
    }

    _running = false;

    if (_console != nullptr && _consoleListenerId >= 0) {
        _console->removeListener(_consoleListenerId);
        _consoleListenerId = -1;
    }

    if (_listenFd >= 0) {
        ::close(_listenFd);
        _listenFd = -1;
    }

    if (_acceptThread.joinable()) {
        _acceptThread.join();
    }

    std::lock_guard<std::mutex> lock(_sessionsMutex);
    for (auto &session : _sessions) {
        session->stop();
    }
    _sessions.clear();
}

bool TelnetServer::isRunning() const
{
    return _running;
}

int TelnetServer::getPort() const
{
    return _port;
}

size_t TelnetServer::getActiveSessionCount() const
{
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    return _sessions.size();
}

void TelnetServer::broadcastConsoleOutput(const char *data, size_t len)
{
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    for (auto &session : _sessions) {
        if (session->isRunning()) {
            session->sendData(data, len);
        }
    }
}

void TelnetServer::pruneDisconnectedSessions()
{
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    _sessions.erase(
        std::remove_if(_sessions.begin(), _sessions.end(),
                       [](const std::shared_ptr<TelnetSession> &s) {
                           return !s->isRunning();
                       }),
        _sessions.end());
}

void TelnetServer::acceptWorker()
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
                auto session = std::make_shared<TelnetSession>(clientFd, clientAddr, _console);
                if (session->start()) {
                    std::lock_guard<std::mutex> lock(_sessionsMutex);
                    _sessions.push_back(session);
                    fprintf(stderr, "[embdevenv] Telnet client connected from %s:%d (port %d)\n",
                            session->getClientIp().c_str(), session->getClientPort(), _port);
                }
            }
        }

        pruneDisconnectedSessions();
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
