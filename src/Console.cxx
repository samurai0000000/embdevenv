/*
 * Console.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Console.hxx"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <sstream>
#include <iomanip>

namespace embdevenv {

namespace {
struct ScopedActivityGuard {
    ScopedActivityGuard(std::function<void(bool)> hook) : _hook(std::move(hook)) {
        if (_hook) _hook(true);
    }
    ~ScopedActivityGuard() {
        if (_hook) _hook(false);
    }
    std::function<void(bool)> _hook;
};
}

Console::Console(const std::string &name, const std::string &port, int baudRate,
                 size_t lineBufferCapacity, int telnetPort)
    : _name(name)
    , _port(port)
    , _baudRate(baudRate)
    , _telnetPort(telnetPort)
    , _serial(_port, _baudRate)
    , _running(false)
    , _connected(false)
    , _lineBuffer(lineBufferCapacity)
    , _lastCharWasCr(false)
    , _nextListenerId(1)
{
}

Console::Console(const std::string &port, int baudRate, size_t lineBufferCapacity)
    : _name("console")
    , _port(port)
    , _baudRate(baudRate)
    , _telnetPort(0)
    , _serial(_port, _baudRate)
    , _running(false)
    , _connected(false)
    , _lineBuffer(lineBufferCapacity)
    , _lastCharWasCr(false)
    , _nextListenerId(1)
{
}

Console::~Console()
{
    stop();
}

bool Console::start()
{
    if (_running) {
        return true;
    }

    _running = true;
    _workerThread = std::thread(&Console::readWorker, this);
    return true;
}

void Console::stop()
{
    if (!_running) {
        return;
    }

    _running = false;
    if (_workerThread.joinable()) {
        _workerThread.join();
    }

    stopLogFile();
    _serial.close();
    _connected = false;
}

bool Console::isRunning() const
{
    return _running;
}

bool Console::isConnected() const
{
    return _connected;
}

ssize_t Console::write(const char *data, size_t len)
{
    if (!_serial.isOpen()) {
        return -1;
    }
    if (_transferInProgress.load()) {
        if (data != nullptr && len > 0) {
            for (size_t i = 0; i < len; ++i) {
                if (data[i] == 0x03) {
                    cancelTransfer();
                    break;
                }
            }
        }
        return -1;
    }
    return _serial.write(data, len);
}

ssize_t Console::writeRaw(const char *data, size_t len)
{
    if (!_serial.isOpen() || data == nullptr || len == 0) {
        return -1;
    }
    return _serial.write(data, len);
}

bool Console::isTransferInProgress() const
{
    return _transferInProgress.load();
}

void Console::cancelTransfer()
{
    _transferCancel.store(true);
}

ssize_t Console::write(const std::string &data)
{
    return write(data.data(), data.size());
}

ssize_t Console::writePaced(const std::string &data, int charDelayMs)
{
    if (charDelayMs <= 0) {
        return write(data);
    }

    if (!_serial.isOpen()) {
        return -1;
    }

    ssize_t totalWritten = 0;
    for (char c : data) {
        ssize_t n = _serial.write(&c, 1);
        if (n > 0) {
            totalWritten += n;
        } else if (n < 0) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(charDelayMs));
    }
    return totalWritten;
}

Console::CatchBootloaderResult Console::catchBootloader(
    const std::string &prompt,
    const std::string &interruptKey,
    int timeoutSec,
    int pollIntervalMs)
{
    ActivityHook hook;
    {
        std::lock_guard<std::mutex> lock(_activityHookMutex);
        hook = _activityHook;
    }
    ScopedActivityGuard activityGuard(hook);

    CatchBootloaderResult result;
    auto startTime = std::chrono::steady_clock::now();

    struct BootloaderContext {
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> active{true};
        bool matched = false;
        std::string streamTail;
        std::string capturedOutput;
    };
    auto ctx = std::make_shared<BootloaderContext>();

    int listenerId = addListener([ctx, prompt](const char *data, size_t len) {
        if (!ctx->active.load()) return;
        std::lock_guard<std::mutex> lock(ctx->mtx);
        if (!ctx->active.load()) return;
        ctx->capturedOutput.append(data, len);
        ctx->streamTail.append(data, len);
        if (ctx->streamTail.size() > 4096) {
            ctx->streamTail.erase(0, ctx->streamTail.size() - 4096);
        }
        if (ctx->streamTail.find(prompt) != std::string::npos) {
            ctx->matched = true;
            ctx->cv.notify_one();
        }
    });

    // Default to 150ms keystroke pacing (instead of 50ms) to prevent USB-UART bridge buffer overflow
    int pollMs = pollIntervalMs > 0 ? pollIntervalMs : 150;
    int maxWaitSec = timeoutSec > 0 ? timeoutSec : 30;
    int consecutiveWriteErrors = 0;

    while (!ctx->matched) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - startTime).count();
        if (elapsed >= maxWaitSec) {
            break;
        }

        if (!_serial.isOpen()) {
            break;
        }

        // Send interrupt key/character into serial with error checking
        ssize_t nWritten = write(interruptKey);
        if (nWritten < 0) {
            consecutiveWriteErrors++;
            if (consecutiveWriteErrors >= 3) {
                // Device write failure (e.g. USB stall or disconnect); abort to avoid crashing USB hub
                break;
            }
        } else {
            consecutiveWriteErrors = 0;
        }

        std::unique_lock<std::mutex> lock(ctx->mtx);
        if (ctx->cv.wait_for(lock, std::chrono::milliseconds(pollMs), [&]{ return ctx->matched; })) {
            break;
        }
    }

    removeListener(listenerId);

    {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->active.store(false);
    }

    auto endTime = std::chrono::steady_clock::now();
    result.elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(endTime - startTime).count();
    result.success = ctx->matched;
    if (ctx->matched) {
        result.matchedPrompt = prompt;
    }
    result.capturedOutput = std::move(ctx->capturedOutput);

    return result;
}

Console::ExpectResult Console::expect(
    const std::string &command,
    const std::vector<std::string> &patterns,
    int timeoutSec,
    int charDelayMs,
    const std::string &logFilePath)
{
    ActivityHook hook;
    {
        std::lock_guard<std::mutex> lock(_activityHookMutex);
        hook = _activityHook;
    }
    ScopedActivityGuard activityGuard(hook);

    ExpectResult result;
    auto startTime = std::chrono::steady_clock::now();

    std::unique_ptr<std::ofstream> logOut;
    if (!logFilePath.empty()) {
        logOut = std::make_unique<std::ofstream>(logFilePath, std::ios::out | std::ios::app);
    }

    struct ExpectContext {
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> active{true};
        bool matched = false;
        std::string matchedPattern;
        std::string streamTail;
        std::string capturedOutput;
        std::ofstream *logOut = nullptr;
    };
    auto ctx = std::make_shared<ExpectContext>();
    ctx->logOut = logOut.get();

    int listenerId = addListener([ctx, &patterns](const char *data, size_t len) {
        if (!ctx->active.load()) return;
        std::lock_guard<std::mutex> lock(ctx->mtx);
        if (!ctx->active.load()) return;
        ctx->capturedOutput.append(data, len);
        if (ctx->logOut && ctx->logOut->is_open()) {
            ctx->logOut->write(data, len);
            ctx->logOut->flush();
        }
        ctx->streamTail.append(data, len);
        if (ctx->streamTail.size() > 8192) {
            ctx->streamTail.erase(0, ctx->streamTail.size() - 8192);
        }
        for (const auto &pat : patterns) {
            if (!pat.empty() && ctx->streamTail.find(pat) != std::string::npos) {
                ctx->matched = true;
                ctx->matchedPattern = pat;
                ctx->cv.notify_one();
                break;
            }
        }
    });

    if (!command.empty()) {
        std::string toSend = command;
        if (toSend.empty() || toSend.back() != '\n') {
            toSend += "\n";
        }
        writePaced(toSend, charDelayMs);
    }

    int maxWaitSec = timeoutSec > 0 ? timeoutSec : 60;
    {
        std::unique_lock<std::mutex> lock(ctx->mtx);
        ctx->cv.wait_for(lock, std::chrono::seconds(maxWaitSec), [&]{ return ctx->matched; });
    }

    removeListener(listenerId);

    {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->active.store(false);
    }

    if (logOut && logOut->is_open()) {
        logOut->close();
    }

    auto endTime = std::chrono::steady_clock::now();
    result.elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(endTime - startTime).count();
    result.success = ctx->matched;
    result.matchedPattern = std::move(ctx->matchedPattern);
    result.capturedOutput = std::move(ctx->capturedOutput);

    return result;
}

void Console::broadcastProgress(const std::string &msg, int excludeListenerId)
{
    if (msg.empty()) {
        return;
    }
    std::vector<OutputCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_listenersMutex);
        callbacks.reserve(_listeners.size());
        for (const auto &pair : _listeners) {
            if (excludeListenerId != 0 && pair.first == excludeListenerId) {
                continue; // Do NOT leak synthetic progress text into the transfer RX queue!
            }
            callbacks.push_back(pair.second);
        }
    }
    for (const auto &cb : callbacks) {
        if (cb) {
            cb(msg.data(), msg.size());
        }
    }
}

Console::TransferFileResult Console::transferFile(
    const std::string &filePath,
    XyModem::Protocol proto,
    bool sendCommand,
    const std::string &address,
    int timeoutSec)
{
    ActivityHook hook;
    {
        std::lock_guard<std::mutex> lock(_activityHookMutex);
        hook = _activityHook;
    }
    ScopedActivityGuard activityGuard(hook);

    TransferFileResult result;
    result.protocol = (proto == XyModem::Protocol::YMODEM) ? "loady" : "loadx";

    if (_transferInProgress.exchange(true)) {
        result.errorMessage = "Console is currently locked for active serial file transfer";
        return result;
    }

    struct TransferGuard {
        std::atomic<bool> &flag;
        ~TransferGuard() { flag.store(false); }
    } guard{_transferInProgress};

    _transferCancel.store(false);

    if (!_serial.isOpen()) {
        result.errorMessage = "Target serial port is not open or disconnected";
        return result;
    }

    // Set up thread-safe RX queue for transfer engine
    struct RxContext {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<uint8_t> queue;
        std::string postTransferOutput;
        bool collectingPostOutput = false;
        std::atomic<bool> active{true};
    };
    auto rxCtx = std::make_shared<RxContext>();

    int listenerId = addListener([rxCtx](const char *data, size_t len) {
        if (!rxCtx->active.load() || data == nullptr || len == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(rxCtx->mtx);
        if (!rxCtx->active.load()) {
            return;
        }
        if (rxCtx->collectingPostOutput) {
            rxCtx->postTransferOutput.append(data, len);
            rxCtx->cv.notify_one();
        } else {
            for (size_t i = 0; i < len; ++i) {
                rxCtx->queue.push_back(static_cast<uint8_t>(data[i]));
            }
            rxCtx->cv.notify_one();
        }
    });

    XyModem::TxFunc txFunc = [this](const char *data, size_t len) -> ssize_t {
        return writeRaw(data, len);
    };

    XyModem::RxFunc rxFunc = [rxCtx](int timeoutMs) -> int {
        std::unique_lock<std::mutex> lock(rxCtx->mtx);
        if (rxCtx->queue.empty()) {
            if (!rxCtx->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]{
                return !rxCtx->queue.empty() || !rxCtx->active.load();
            })) {
                return -1;
            }
        }
        if (rxCtx->queue.empty()) {
            return -1;
        }
        uint8_t b = rxCtx->queue.front();
        rxCtx->queue.pop_front();
        return static_cast<int>(b);
    };

    auto transferStartTime = std::chrono::steady_clock::now();
    int lastPercentBroadcast = -1;
    std::string protoUpper = (proto == XyModem::Protocol::YMODEM) ? "YMODEM" : "XMODEM";

    XyModem::ProgressFunc progressFunc = [this, listenerId, &protoUpper, transferStartTime, &lastPercentBroadcast](
        size_t bytesSent, size_t totalBytes, int packetsSent) {
        int percent = totalBytes > 0 ? static_cast<int>((bytesSent * 100) / totalBytes) : 0;
        if (percent / 10 > lastPercentBroadcast / 10 || bytesSent == totalBytes) {
            bool isFirst = (lastPercentBroadcast < 0);
            lastPercentBroadcast = percent;
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - transferStartTime).count();
            double rate = elapsed > 0.001 ? (bytesSent / 1024.0) / elapsed : 0.0;
            std::ostringstream oss;
            if (isFirst) {
                oss << "\r\n";
            }
            oss << "[embdevenv] " << protoUpper << " progress: " << percent << "% ("
                << (bytesSent / 1024) << " KB, " << std::fixed << std::setprecision(1) << rate << " KB/s)\r\n";
            broadcastProgress(oss.str(), listenerId);
        }
    };

    XyModem modem(txFunc, rxFunc, progressFunc);

    if (sendCommand) {
        std::string cmd = (proto == XyModem::Protocol::YMODEM) ? "loady" : "loadx";
        if (!address.empty()) {
            cmd += " " + address;
        }
        broadcastProgress("\r\n[embdevenv] Sending bootloader command: " + cmd + "\r\n", listenerId);
        std::string rawCmd = cmd + "\n";
        writeRaw(rawCmd.data(), rawCmd.size());
    } else {
        broadcastProgress("\r\n", listenerId);
    }

    broadcastProgress("[embdevenv] Starting " + protoUpper + " transfer: '" + filePath + "'\r\n", listenerId);

    auto modemResult = modem.send(proto, filePath, _transferCancel);

    result.success = modemResult.success;
    result.fileSize = modemResult.fileSize;
    result.bytesSent = modemResult.bytesSent;
    result.packetsSent = modemResult.packetsSent;
    result.retries = modemResult.retries;
    result.elapsedSec = modemResult.elapsedSec;
    result.transferRateKBps = modemResult.transferRateKBps;
    result.errorMessage = modemResult.errorMessage;

    // Drain post-transfer output from target bootloader (e.g. "## Total Size = ...")
    {
        std::unique_lock<std::mutex> lock(rxCtx->mtx);
        rxCtx->collectingPostOutput = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        size_t prevLen = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            rxCtx->cv.wait_for(lock, std::chrono::milliseconds(200));
            if (!rxCtx->postTransferOutput.empty() && rxCtx->postTransferOutput.size() == prevLen) {
                break;
            }
            prevLen = rxCtx->postTransferOutput.size();
        }
    }

    // Normalize newlines in capturedOutput (\r\r\n -> \n, \r\n -> \n) and strip leading/trailing control chars/whitespace
    std::string cleanOutput;
    cleanOutput.reserve(rxCtx->postTransferOutput.size());
    for (size_t i = 0; i < rxCtx->postTransferOutput.size(); ++i) {
        char c = rxCtx->postTransferOutput[i];
        if (c == '\r') {
            if (i + 1 < rxCtx->postTransferOutput.size() &&
                (rxCtx->postTransferOutput[i + 1] == '\r' || rxCtx->postTransferOutput[i + 1] == '\n')) {
                continue;
            }
            cleanOutput += '\n';
        } else {
            cleanOutput += c;
        }
    }
    while (!cleanOutput.empty() && static_cast<unsigned char>(cleanOutput.front()) < 0x20 &&
           cleanOutput.front() != '\n' && cleanOutput.front() != '\t') {
        cleanOutput.erase(0, 1);
    }
    while (!cleanOutput.empty() && (cleanOutput.front() == '\r' || cleanOutput.front() == '\n')) {
        cleanOutput.erase(0, 1);
    }
    while (!cleanOutput.empty() && (cleanOutput.back() == '\r' || cleanOutput.back() == '\n' || cleanOutput.back() == ' ')) {
        cleanOutput.pop_back();
    }
    result.capturedOutput = cleanOutput;

    removeListener(listenerId);
    {
        std::lock_guard<std::mutex> lock(rxCtx->mtx);
        rxCtx->active.store(false);
    }

    if (result.success) {
        std::ostringstream oss;
        oss << "[embdevenv] " << protoUpper << " transfer complete: "
            << result.bytesSent << " bytes in "
            << std::fixed << std::setprecision(2) << result.elapsedSec << "s ("
            << std::fixed << std::setprecision(1) << result.transferRateKBps << " KB/s)\r\n";
        broadcastProgress(oss.str(), listenerId);
    } else {
        broadcastProgress("[embdevenv] " + protoUpper + " transfer failed: " +
                          result.errorMessage + "\r\n", listenerId);
    }

    return result;
}

bool Console::startLogFile(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(_fileLogMutex);
    stopLogFileLocked();

    _fileLogStream = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app);
    if (!_fileLogStream->is_open()) {
        _fileLogStream.reset();
        return false;
    }

    _fileLogPath = path;
    _fileLogListenerId = addListener([this](const char *data, size_t len) {
        std::lock_guard<std::recursive_mutex> l(_fileLogMutex);
        if (_fileLogStream && _fileLogStream->is_open()) {
            _fileLogStream->write(data, len);
            _fileLogStream->flush();
        }
    });

    return true;
}

void Console::stopLogFile()
{
    std::lock_guard<std::recursive_mutex> lock(_fileLogMutex);
    stopLogFileLocked();
}

void Console::stopLogFileLocked()
{
    if (_fileLogListenerId > 0) {
        removeListener(_fileLogListenerId);
        _fileLogListenerId = 0;
    }
    if (_fileLogStream) {
        if (_fileLogStream->is_open()) {
            _fileLogStream->close();
        }
        _fileLogStream.reset();
    }
    _fileLogPath.clear();
}

bool Console::isLoggingToFile() const
{
    return _fileLogListenerId > 0;
}

std::vector<std::string> Console::getRecentLines(size_t count) const
{
    std::lock_guard<std::mutex> lock(_lineMutex);
    std::vector<std::string> lines = _lineBuffer.getLatest(count);
    if (!_partialLine.empty()) {
        lines.push_back(_partialLine);
    }
    return lines;
}

void Console::clearBuffer()
{
    std::lock_guard<std::mutex> lock(_lineMutex);
    _lineBuffer.clear();
    _partialLine.clear();
    _lastCharWasCr = false;
}

int Console::addListener(OutputCallback callback)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    int id = _nextListenerId++;
    _listeners.emplace_back(id, std::move(callback));
    return id;
}

void Console::removeListener(int listenerId)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
                       [listenerId](const std::pair<int, OutputCallback> &item) {
                           return item.first == listenerId;
                       }),
        _listeners.end());
}

const std::string &Console::getName() const
{
    return _name;
}

const std::string &Console::getPort() const
{
    if (!_serial.getDevice().empty()) {
        return _serial.getDevice();
    }
    return _port;
}

int Console::getBaudRate() const
{
    return _baudRate;
}

int Console::getTelnetPort() const
{
    return _telnetPort;
}

void Console::setUsbBinding(const std::string &usbPath, int portIndex)
{
    _serial.setUsbBinding(usbPath, portIndex);
}

void Console::setActivityHook(ActivityHook hook)
{
    std::lock_guard<std::mutex> lock(_activityHookMutex);
    _activityHook = std::move(hook);
}

void Console::readWorker()
{
    char buf[512];

    while (_running) {
        if (!_serial.isOpen()) {
            if (_serial.open(_port, _baudRate)) {
                _connected = true;
                _port = _serial.getDevice();
            } else {
                _connected = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        ssize_t n = _serial.read(buf, sizeof(buf), 100);
        if (n > 0) {
            // Broadcast raw bytes to all listeners (e.g. Telnet clients)
            {
                std::vector<OutputCallback> callbacks;
                {
                    std::lock_guard<std::mutex> lock(_listenersMutex);
                    callbacks.reserve(_listeners.size());
                    for (const auto &pair : _listeners) {
                        callbacks.push_back(pair.second);
                    }
                }
                for (const auto &cb : callbacks) {
                    if (cb) {
                        cb(buf, static_cast<size_t>(n));
                    }
                }
            }

            // Buffer lines for web UI / logging
            {
                std::lock_guard<std::mutex> lock(_lineMutex);
                for (ssize_t i = 0; i < n; ++i) {
                    char c = buf[i];
                    if (_transferInProgress.load()) {
                        if (static_cast<unsigned char>(c) < 0x20 && c != '\r' && c != '\n' && c != '\t') {
                            continue;
                        }
                    }
                    if (c == '\n') {
                        if (_lastCharWasCr) {
                            // \r was already handled; ignore matching \n in \r\n pair
                            _lastCharWasCr = false;
                        } else {
                            // Standalone \n
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                        }
                    } else if (c == '\r') {
                        if (!_lastCharWasCr) {
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                            _lastCharWasCr = true;
                        }
                    } else {
                        _lastCharWasCr = false;
                        _partialLine += c;
                        // Avoid unbounded partial line growth if no newline occurs
                        if (_partialLine.size() > 4096) {
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                        }
                    }
                }
            }
        } else if (n < 0) {
            // Read error or device disconnected
            _connected = false;
            _serial.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
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
