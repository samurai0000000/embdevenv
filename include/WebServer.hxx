/*
 * WebServer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_WEB_SERVER_HXX
#define EMBDEVENV_WEB_SERVER_HXX

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include <condition_variable>

namespace httplib {
class Server;
class Request;
}

namespace embdevenv {

class WebServer {
public:
    WebServer(int port = 3886, const std::string &bindAddress = "0.0.0.0", bool endpointsEnabled = false);
    ~WebServer();

    bool start();
    void stop();
    bool isRunning() const;

    int getPort() const;
    const std::string &getBindAddress() const;
    bool areEndpointsEnabled() const;
    void setEndpointsEnabled(bool enabled);

private:
    void serverWorker();
    void setupRoutes();
    std::string getDashboardHtml(const std::string &sessionToken = "") const;

    std::string createUiSession();
    bool isValidUiSession(const httplib::Request &req) const;

    int _port;
    std::string _bindAddress;
    bool _endpointsEnabled;
    std::unique_ptr<httplib::Server> _server;
    std::atomic<bool> _running;
    std::thread _serverThread;

    mutable std::mutex _sessionMutex;
    std::map<std::string, time_t> _uiSessions;

    struct McpSessionState {
        std::string sessionId;
        time_t lastActivity = 0;
        std::vector<std::string> pendingMessages;
        bool closed = false;
    };

    mutable std::mutex _mcpMutex;
    std::condition_variable _mcpCv;
    std::map<std::string, McpSessionState> _mcpSessions;
};

} // namespace embdevenv

#endif // EMBDEVENV_WEB_SERVER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
