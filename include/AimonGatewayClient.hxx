/*
 * AimonGatewayClient.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_AIMON_GATEWAY_CLIENT_HXX
#define EMBDEVENV_AIMON_GATEWAY_CLIENT_HXX

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace embdevenv {

class AimonGatewayClient {
public:
    static AimonGatewayClient &getInstance();

    bool start(const std::string &host = "", int port = 0);
    void stop();
    void join();

    bool isConnected() const;

private:
    AimonGatewayClient();
    ~AimonGatewayClient();
    AimonGatewayClient(const AimonGatewayClient &) = delete;
    AimonGatewayClient &operator=(const AimonGatewayClient &) = delete;

    void run();
    bool connectToGateway();
    void disconnect();
    bool sendRegistration();
    void processIncoming();

    void handleRequest(const std::string &line);
    void sendResponse(const std::string &jsonResponse);

    std::string _host;
    int _port;

    std::atomic<bool> _running;
    std::atomic<bool> _connected;
    int _sockFd;

    std::thread _thread;
    std::mutex _sendMutex;
};

} // namespace embdevenv

#endif // EMBDEVENV_AIMON_GATEWAY_CLIENT_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
