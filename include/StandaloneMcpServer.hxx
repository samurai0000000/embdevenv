/*
 * StandaloneMcpServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_STANDALONE_MCP_SERVER_HXX
#define EMBDEVENV_STANDALONE_MCP_SERVER_HXX

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <netinet/in.h>

namespace embdevenv {

class StandaloneMcpSession {
public:
    StandaloneMcpSession(int clientFd, struct sockaddr_in addr);
    ~StandaloneMcpSession();

    bool start();
    void stop();
    bool isRunning() const;

private:
    void sessionWorker();
    void handleLine(const std::string &line);
    void sendResponse(const std::string &msg);

    int _clientFd;
    struct sockaddr_in _addr;
    std::atomic<bool> _running;
    std::thread _workerThread;
    std::mutex _writeMutex;
};

class StandaloneMcpServer {
public:
    explicit StandaloneMcpServer(int port = 3889);
    ~StandaloneMcpServer();

    bool start();
    void stop();
    bool isRunning() const;

    int getPort() const;

private:
    void acceptWorker();
    void pruneSessions();

    int _port;
    int _listenFd;
    std::atomic<bool> _running;
    std::thread _acceptThread;

    mutable std::mutex _sessionsMutex;
    std::vector<std::shared_ptr<StandaloneMcpSession>> _sessions;
};

} // namespace embdevenv

#endif // EMBDEVENV_STANDALONE_MCP_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
