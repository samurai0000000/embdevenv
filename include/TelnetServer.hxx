/*
 * TelnetServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TELNET_SERVER_HXX
#define EMBDEVENV_TELNET_SERVER_HXX

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include "TelnetBackend.hxx"

namespace embdevenv {

class TelnetSession {
public:
    TelnetSession(int clientFd, struct sockaddr_in addr, ITelnetBackend *backend);
    ~TelnetSession();

    bool start();
    void stop();
    bool isRunning() const;

    void sendData(const char *data, size_t len);
    std::string getClientIp() const;
    int getClientPort() const;

private:
    void sessionWorker();
    void processIncomingBytes(const char *buf, size_t len);

    int _clientFd;
    struct sockaddr_in _addr;
    ITelnetBackend *_backend;
    std::atomic<bool> _running;
    std::thread _workerThread;
    mutable std::mutex _writeMutex;
};

class TelnetServer {
public:
    TelnetServer(int port, ITelnetBackend *backend);
    ~TelnetServer();

    bool start();
    void stop();
    bool isRunning() const;

    int getPort() const;
    size_t getActiveSessionCount() const;

private:
    void acceptWorker();
    void broadcastConsoleOutput(const char *data, size_t len);
    void pruneDisconnectedSessions();

    int _port;
    ITelnetBackend *_backend;
    int _listenFd;
    int _listenerId;

    std::atomic<bool> _running;
    std::thread _acceptThread;

    mutable std::mutex _sessionsMutex;
    std::vector<std::shared_ptr<TelnetSession>> _sessions;
};

} // namespace embdevenv

#endif // EMBDEVENV_TELNET_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
