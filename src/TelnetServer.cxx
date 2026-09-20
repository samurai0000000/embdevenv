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
#include <cctype>
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

TelnetSession::TelnetSession(int clientFd, struct sockaddr_in addr, ITelnetBackend *backend)
    : _clientFd(clientFd)
    , _addr(addr)
    , _backend(backend)
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
    _running = false;
    if (_clientFd >= 0) {
        ::shutdown(_clientFd, SHUT_RDWR);
        ::close(_clientFd);
        _clientFd = -1;
    }

    if (_workerThread.joinable()) {
        if (_workerThread.get_id() != std::this_thread::get_id()) {
            _workerThread.join();
        } else {
            _workerThread.detach();
        }
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
    if (_backend == nullptr || buf == nullptr || len == 0) {
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
        } else if (c == 0x1b && i + 1 < len && buf[i + 1] == '[') {
            // Check for automated terminal emulator responses such as:
            // - Cursor Position Report (CPR): \x1b[<row>;<col>R or \x1b[<row>R
            // - Device Attributes (DA): \x1b[?<attrs>c or \x1b[<attrs>c
            size_t j = i + 2;
            if (j < len && buf[j] == '?') {
                ++j;
            }
            size_t startDigits = j;
            while (j < len && (std::isdigit(static_cast<unsigned char>(buf[j])) || buf[j] == ';')) {
                ++j;
            }
            if (j > startDigits && j < len && (buf[j] == 'R' || buf[j] == 'c')) {
                // Drop automated terminal emulator response (e.g. \x1b[28R, \x1b[20R)
                // so it does not corrupt the target's serial console input.
                i = j + 1;
                continue;
            }
            cleanData += static_cast<char>(c);
            ++i;
        } else {
            cleanData += static_cast<char>(c);
            ++i;
        }
    }

    if (!cleanData.empty()) {
        if (_backend->isTransferInProgress()) {
            if (cleanData.find('\x03') != std::string::npos) {
                _backend->cancelTransfer();
                static const std::string abortMsg = "\r\n[embdevenv] Serial transfer aborted by user.\r\n";
                sendData(abortMsg.data(), abortMsg.size());
            } else {
                static const std::string lockMsg = "\r\n[embdevenv] Serial transfer in progress. Input locked (Ctrl-C to abort).\r\n";
                sendData(lockMsg.data(), lockMsg.size());
            }
        } else {
            _backend->write(cleanData);
        }
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

TelnetServer::TelnetServer(int port, ITelnetBackend *backend)
    : _port(port)
    , _backend(backend)
    , _listenFd(-1)
    , _listenerId(-1)
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

    // Register callback with backend to receive output
    if (_backend != nullptr) {
        _listenerId = _backend->addListener(
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

    if (_backend != nullptr && _listenerId >= 0) {
        _backend->removeListener(_listenerId);
        _listenerId = -1;
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
    for (auto it = _sessions.begin(); it != _sessions.end(); ) {
        if (!(*it)->isRunning()) {
            if (_backend != nullptr) {
                _backend->onClientDisconnect();
            }
            it = _sessions.erase(it);
        } else {
            ++it;
        }
    }
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
                auto session = std::make_shared<TelnetSession>(clientFd, clientAddr, _backend);
                if (session->start()) {
                    if (_backend != nullptr) {
                        _backend->onClientConnect();
                    }
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
