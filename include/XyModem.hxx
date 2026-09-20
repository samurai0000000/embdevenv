/*
 * XyModem.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_XY_MODEM_HXX
#define EMBDEVENV_XY_MODEM_HXX

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <sys/types.h>

namespace embdevenv {

class XyModem {
public:
    enum class Protocol {
        YMODEM,
        XMODEM
    };

    // TX: write raw bytes; returns number of bytes written or -1 on error
    using TxFunc = std::function<ssize_t(const char *data, size_t len)>;
    // RX: read one byte with timeout in ms; returns byte (0-255) or -1 on timeout/error
    using RxFunc = std::function<int(int timeoutMs)>;
    // Progress: callback for UI/logging (bytesSent, totalBytes, packetsSent)
    using ProgressFunc = std::function<void(size_t bytesSent, size_t totalBytes, int packetsSent)>;

    struct TransferResult {
        bool success = false;
        std::string protocol;
        size_t fileSize = 0;
        size_t bytesSent = 0;
        int packetsSent = 0;
        int retries = 0;
        double elapsedSec = 0.0;
        double transferRateKBps = 0.0;
        std::string errorMessage;
    };

    // Protocol control characters
    static constexpr uint8_t SOH    = 0x01; // 128-byte block
    static constexpr uint8_t STX    = 0x02; // 1024-byte block
    static constexpr uint8_t EOT    = 0x04; // End of transmission
    static constexpr uint8_t ACK    = 0x06; // Acknowledge
    static constexpr uint8_t NAK    = 0x15; // Negative acknowledge
    static constexpr uint8_t CAN    = 0x18; // Cancel
    static constexpr uint8_t CHAR_C = 0x43; // 'C' (request CRC-16 mode)
    static constexpr uint8_t CPMEOF = 0x1a; // Ctrl-Z padding

    XyModem(TxFunc tx, RxFunc rx, ProgressFunc progress = nullptr);

    TransferResult send(Protocol proto,
                        const std::string &filePath,
                        std::atomic<bool> &cancelFlag);

    static uint16_t crc16(const uint8_t *data, size_t len);
    static uint8_t checksum8(const uint8_t *data, size_t len);

private:
    void sendCancel();
    TransferResult sendYmodem(const std::string &filePath, std::atomic<bool> &cancelFlag);
    TransferResult sendXmodem(const std::string &filePath, std::atomic<bool> &cancelFlag);

    TxFunc _tx;
    RxFunc _rx;
    ProgressFunc _progress;
};

} // namespace embdevenv

#endif // EMBDEVENV_XY_MODEM_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
