/*
 * N1655Mcu.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_N1655_MCU_HXX
#define EMBDEVENV_N1655_MCU_HXX

#include "Mcu.hxx"
#include "SerialPort.hxx"
#include "TelemetryStore.hxx"
#include "TelnetBackend.hxx"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>

namespace embdevenv {

class N1655Mcu : public Mcu, public ITelnetBackend {
public:
    N1655Mcu(const std::string &port, int baudRate = 115200, int pollIntervalMs = 2000, int telnetPort = 0);
    ~N1655Mcu() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;
    bool isConnected() const override;

    bool powerOn() override;
    bool powerOff() override;
    bool reboot() override;
    bool resetUsbDownload(bool doubleReset = true) override;
    std::string sendCommand(const std::string &cmd, int timeoutMs = 2000) override;

    ssize_t write(const char *data, size_t len) override;
    ssize_t write(const std::string &data) override;
    int addListener(OutputCallback callback) override;
    void removeListener(int listenerId) override;
    void onClientConnect() override;
    void onClientDisconnect() override;

    std::shared_ptr<TelemetryStore> getTelemetryStore() const override;
    nlohmann::json getStatusJson() const override;

    const std::string &getPort() const override;
    int getBaudRate() const override;
    int getPollIntervalMs() const;
    int getTelnetPort() const;

    void setUsbBinding(const std::string &usbPath, int portIndex);

private:
    void pollWorker();
    bool recoverPrompt(int timeoutMs = 1000);
    bool parseTelemetryBlock(const std::string &block, TelemetrySample &sample);
    void broadcastOutput(const char *data, size_t len);

    std::string _port;
    int _baudRate;
    int _pollIntervalMs;
    int _telnetPort;

    SerialPort _serial;
    std::shared_ptr<TelemetryStore> _telemetryStore;

    std::atomic<bool> _running;
    std::atomic<bool> _connected;
    std::atomic<int> _activeClients;
    std::thread _workerThread;
    mutable std::mutex _cmdMutex;
    std::condition_variable _cv;

    mutable std::mutex _listenersMutex;
    int _nextListenerId;
    std::vector<std::pair<int, OutputCallback>> _listeners;
};

} // namespace embdevenv

#endif // EMBDEVENV_N1655_MCU_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
