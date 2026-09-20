/*
 * XyModem.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "XyModem.hxx"
#include <fstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace embdevenv {

XyModem::XyModem(TxFunc tx, RxFunc rx, ProgressFunc progress)
    : _tx(std::move(tx))
    , _rx(std::move(rx))
    , _progress(std::move(progress))
{
}

uint16_t XyModem::crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

uint8_t XyModem::checksum8(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xff);
}

void XyModem::sendCancel()
{
    const char canSeq[] = {
        static_cast<char>(CAN), static_cast<char>(CAN),
        static_cast<char>(CAN), static_cast<char>(CAN),
        static_cast<char>(CAN),
        '\b', '\b', '\b', '\b', '\b',
        '\r', '\n'
    };
    if (_tx) {
        _tx(canSeq, sizeof(canSeq));
    }
}

static int readControlResponse(const XyModem::RxFunc &rx, std::atomic<bool> &cancelFlag, int timeoutSec = 5)
{
    auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count() < timeoutSec) {
        if (cancelFlag.load()) {
            return -2; // Cancelled
        }
        int resp = rx(1000);
        if (resp == XyModem::ACK || resp == XyModem::NAK || resp == XyModem::CAN) {
            return resp;
        }
    }
    return -1; // Timeout
}

XyModem::TransferResult XyModem::send(Protocol proto,
                                     const std::string &filePath,
                                     std::atomic<bool> &cancelFlag)
{
    if (proto == Protocol::YMODEM) {
        return sendYmodem(filePath, cancelFlag);
    } else {
        return sendXmodem(filePath, cancelFlag);
    }
}

XyModem::TransferResult XyModem::sendYmodem(const std::string &filePath,
                                           std::atomic<bool> &cancelFlag)
{
    TransferResult result;
    result.protocol = "loady";
    auto startTime = std::chrono::steady_clock::now();

    std::filesystem::path p(filePath);
    std::string filename = p.filename().string();
    if (filename.empty()) {
        result.errorMessage = "Invalid file path: " + filePath;
        return result;
    }

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.errorMessage = "Failed to open file: " + filePath;
        return result;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    result.fileSize = fileSize;

    // Phase 1: Wait for initial handshake 'C' from receiver
    // Bootloaders might take a few seconds to print banner before emitting 'C'
    bool syncReceived = false;
    auto syncStart = std::chrono::steady_clock::now();
    while (!syncReceived) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        auto now = std::chrono::steady_clock::now();
        double waitSec = std::chrono::duration<double>(now - syncStart).count();
        if (waitSec > 30.0) {
            result.errorMessage = "Handshake timeout waiting for 'C' from receiver";
            return result;
        }

        int b = _rx(1000);
        if (b == CHAR_C) {
            syncReceived = true;
            break;
        } else if (b == CAN) {
            // Check for double CAN
            int b2 = _rx(500);
            if (b2 == CAN) {
                result.errorMessage = "Transfer cancelled by receiver (CAN)";
                return result;
            }
        }
    }

    // Phase 2: Send Packet 0 (Header block with filename & file size)
    // SOH, seq 0, ~seq 0xFF, 128-byte payload, CRC-16
    {
        uint8_t payload[128];
        std::memset(payload, 0, sizeof(payload));

        // Format: <filename>\0<filesize_decimal>\0
        std::string sizeStr = std::to_string(fileSize);
        size_t nameLen = std::min(filename.size(), static_cast<size_t>(100));
        std::memcpy(payload, filename.data(), nameLen);
        payload[nameLen] = '\0';
        size_t sizeOffset = nameLen + 1;
        if (sizeOffset + sizeStr.size() < sizeof(payload)) {
            std::memcpy(payload + sizeOffset, sizeStr.data(), sizeStr.size());
            payload[sizeOffset + sizeStr.size()] = '\0';
        }

        uint16_t crc = crc16(payload, sizeof(payload));
        std::vector<uint8_t> pkt(3 + 128 + 2);
        pkt[0] = SOH;
        pkt[1] = 0x00;
        pkt[2] = 0xFF;
        std::memcpy(pkt.data() + 3, payload, 128);
        pkt[131] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        pkt[132] = static_cast<uint8_t>(crc & 0xFF);

        bool pkt0Acked = false;
        int pkt0Retries = 0;
        while (!pkt0Acked && pkt0Retries < 10) {
            if (cancelFlag.load()) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            }

            ssize_t txRet = _tx(reinterpret_cast<const char*>(pkt.data()), pkt.size());
            if (txRet < 0) {
                result.errorMessage = "Serial write failure sending Packet 0";
                return result;
            }

            int resp = readControlResponse(_rx, cancelFlag, 5);
            if (resp == ACK) {
                pkt0Acked = true;
                break;
            } else if (resp == -2) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            } else if (resp == CAN) {
                result.errorMessage = "Transfer cancelled by receiver (CAN on Packet 0)";
                return result;
            } else {
                pkt0Retries++;
                result.retries++;
            }
        }

        if (!pkt0Acked) {
            sendCancel();
            result.errorMessage = "Packet 0 not acknowledged by receiver";
            return result;
        }

        // Wait for 'C' after Packet 0 ACK (receiver initiates data phase)
        bool dataSync = false;
        auto dataSyncStart = std::chrono::steady_clock::now();
        while (!dataSync) {
            if (cancelFlag.load()) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - dataSyncStart).count() > 10.0) {
                sendCancel();
                result.errorMessage = "Timeout waiting for data phase 'C' after Packet 0";
                return result;
            }

            int b = _rx(1000);
            if (b == CHAR_C) {
                dataSync = true;
                break;
            } else if (b == CAN) {
                result.errorMessage = "Transfer cancelled by receiver (CAN after Packet 0)";
                return result;
            }
        }
    }

    // Phase 3: Send data blocks (1024-byte STX blocks)
    uint8_t seq = 1;
    size_t totalBytesSent = 0;
    int packetsSent = 0;

    while (totalBytesSent < fileSize || (fileSize == 0 && packetsSent == 0)) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        uint8_t payload[1024];
        std::memset(payload, CPMEOF, sizeof(payload));

        size_t toRead = std::min(static_cast<size_t>(1024), fileSize - totalBytesSent);
        if (toRead > 0) {
            file.read(reinterpret_cast<char*>(payload), toRead);
            size_t bytesRead = static_cast<size_t>(file.gcount());
            if (bytesRead < toRead) {
                toRead = bytesRead;
            }
        }

        uint16_t crc = crc16(payload, sizeof(payload));
        std::vector<uint8_t> pkt(3 + 1024 + 2);
        pkt[0] = STX;
        pkt[1] = seq;
        pkt[2] = static_cast<uint8_t>(~seq);
        std::memcpy(pkt.data() + 3, payload, 1024);
        pkt[1027] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        pkt[1028] = static_cast<uint8_t>(crc & 0xFF);

        bool blockAcked = false;
        int blockRetries = 0;
        while (!blockAcked && blockRetries < 10) {
            if (cancelFlag.load()) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            }

            ssize_t txRet = _tx(reinterpret_cast<const char*>(pkt.data()), pkt.size());
            if (txRet < 0) {
                result.errorMessage = "Serial write failure on packet " + std::to_string(packetsSent + 1);
                return result;
            }

            int resp = readControlResponse(_rx, cancelFlag, 5);
            if (resp == ACK) {
                blockAcked = true;
                break;
            } else if (resp == -2) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            } else if (resp == CAN) {
                int resp2 = _rx(500);
                if (resp2 == CAN) {
                    result.errorMessage = "Transfer cancelled by receiver (CAN on packet " +
                                          std::to_string(packetsSent + 1) + ")";
                    return result;
                }
                blockRetries++;
                result.retries++;
            } else {
                blockRetries++;
                result.retries++;
            }
        }

        if (!blockAcked) {
            sendCancel();
            result.errorMessage = "Retry limit exceeded on packet " + std::to_string(packetsSent + 1);
            return result;
        }

        totalBytesSent += toRead;
        packetsSent++;
        seq = static_cast<uint8_t>((seq + 1) & 0xFF);
        result.bytesSent = totalBytesSent;
        result.packetsSent = packetsSent;

        if (_progress) {
            _progress(totalBytesSent, fileSize, packetsSent);
        }

        if (fileSize == 0) {
            break;
        }
    }

    // Phase 4: EOT Handshake
    // Send EOT -> expect NAK -> send EOT -> expect ACK -> expect 'C'
    bool eotCompleted = false;
    for (int eotRound = 0; eotRound < 5 && !eotCompleted; ++eotRound) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        const char eotByte = static_cast<char>(EOT);
        _tx(&eotByte, 1);

        int resp = _rx(5000);
        if (resp == NAK) {
            // Standard YMODEM: receiver responds to first EOT with NAK; send second EOT
            _tx(&eotByte, 1);
            int resp2 = _rx(5000);
            if (resp2 == ACK) {
                eotCompleted = true;
                break;
            }
        } else if (resp == ACK) {
            // Some bootloaders respond directly with ACK to first EOT
            eotCompleted = true;
            break;
        } else if (resp == CAN) {
            result.errorMessage = "Transfer cancelled by receiver during EOT";
            return result;
        }
    }

    if (!eotCompleted) {
        sendCancel();
        result.errorMessage = "Failed EOT handshake with receiver";
        return result;
    }

    // Wait for 'C' requesting the next file or end-of-batch
    int postEotByte = _rx(5000);
    if (postEotByte != CHAR_C && postEotByte != -1) {
        // Some implementations might have flushed, check again
        if (postEotByte != ACK) {
            postEotByte = _rx(2000);
        }
    }

    // Phase 5: End of Batch (Empty Packet 0: 128 bytes of 0x00)
    {
        uint8_t emptyPayload[128];
        std::memset(emptyPayload, 0, sizeof(emptyPayload));
        uint16_t crc = crc16(emptyPayload, sizeof(emptyPayload));

        std::vector<uint8_t> emptyPkt(3 + 128 + 2);
        emptyPkt[0] = SOH;
        emptyPkt[1] = 0x00;
        emptyPkt[2] = 0xFF;
        std::memcpy(emptyPkt.data() + 3, emptyPayload, 128);
        emptyPkt[131] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        emptyPkt[132] = static_cast<uint8_t>(crc & 0xFF);

        _tx(reinterpret_cast<const char*>(emptyPkt.data()), emptyPkt.size());
        _rx(3000); // Read final ACK (ignore timeout if receiver immediately returns to prompt)
    }

    auto endTime = std::chrono::steady_clock::now();
    result.elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    if (result.elapsedSec > 0.001) {
        result.transferRateKBps = (result.bytesSent / 1024.0) / result.elapsedSec;
    }
    result.success = true;
    return result;
}

XyModem::TransferResult XyModem::sendXmodem(const std::string &filePath,
                                           std::atomic<bool> &cancelFlag)
{
    TransferResult result;
    result.protocol = "loadx";
    auto startTime = std::chrono::steady_clock::now();

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.errorMessage = "Failed to open file: " + filePath;
        return result;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    result.fileSize = fileSize;

    // Phase 1: Wait for sync byte from receiver
    // 'C' for CRC mode, NAK for legacy checksum mode
    bool useCrc = true;
    bool syncReceived = false;
    auto syncStart = std::chrono::steady_clock::now();
    while (!syncReceived) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - syncStart).count() > 30.0) {
            result.errorMessage = "Handshake timeout waiting for sync byte ('C' or NAK)";
            return result;
        }

        int b = _rx(1000);
        if (b == CHAR_C) {
            useCrc = true;
            syncReceived = true;
            break;
        } else if (b == NAK) {
            useCrc = false;
            syncReceived = true;
            break;
        } else if (b == CAN) {
            int b2 = _rx(500);
            if (b2 == CAN) {
                result.errorMessage = "Transfer cancelled by receiver (CAN)";
                return result;
            }
        }
    }

    // Phase 2: Send data packets starting directly at seq 1
    uint8_t seq = 1;
    size_t totalBytesSent = 0;
    int packetsSent = 0;

    while (totalBytesSent < fileSize || (fileSize == 0 && packetsSent == 0)) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        // Use 1024-byte STX blocks for XMODEM-1K if >= 1024 bytes remain, else 128-byte SOH
        size_t remaining = fileSize - totalBytesSent;
        size_t blockSize = (remaining >= 1024) ? 1024 : 128;
        uint8_t blockType = (blockSize == 1024) ? STX : SOH;

        std::vector<uint8_t> payload(blockSize, CPMEOF);
        size_t toRead = std::min(blockSize, remaining);
        if (toRead > 0) {
            file.read(reinterpret_cast<char*>(payload.data()), toRead);
            size_t bytesRead = static_cast<size_t>(file.gcount());
            if (bytesRead < toRead) {
                toRead = bytesRead;
            }
        }

        size_t checkLen = useCrc ? 2 : 1;
        std::vector<uint8_t> pkt(3 + blockSize + checkLen);
        pkt[0] = blockType;
        pkt[1] = seq;
        pkt[2] = static_cast<uint8_t>(~seq);
        std::memcpy(pkt.data() + 3, payload.data(), blockSize);

        if (useCrc) {
            uint16_t crc = crc16(payload.data(), blockSize);
            pkt[3 + blockSize] = static_cast<uint8_t>((crc >> 8) & 0xFF);
            pkt[3 + blockSize + 1] = static_cast<uint8_t>(crc & 0xFF);
        } else {
            pkt[3 + blockSize] = checksum8(payload.data(), blockSize);
        }

        bool blockAcked = false;
        int blockRetries = 0;
        while (!blockAcked && blockRetries < 10) {
            if (cancelFlag.load()) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            }

            ssize_t txRet = _tx(reinterpret_cast<const char*>(pkt.data()), pkt.size());
            if (txRet < 0) {
                result.errorMessage = "Serial write failure on packet " + std::to_string(packetsSent + 1);
                return result;
            }

            int resp = readControlResponse(_rx, cancelFlag, 5);
            if (resp == ACK) {
                blockAcked = true;
                break;
            } else if (resp == -2) {
                sendCancel();
                result.errorMessage = "Transfer cancelled by operator";
                return result;
            } else if (resp == CAN) {
                result.errorMessage = "Transfer cancelled by receiver (CAN on packet " +
                                      std::to_string(packetsSent + 1) + ")";
                return result;
            } else {
                blockRetries++;
                result.retries++;
            }
        }

        if (!blockAcked) {
            sendCancel();
            result.errorMessage = "Retry limit exceeded on packet " + std::to_string(packetsSent + 1);
            return result;
        }

        totalBytesSent += toRead;
        packetsSent++;
        seq = static_cast<uint8_t>((seq + 1) & 0xFF);
        result.bytesSent = totalBytesSent;
        result.packetsSent = packetsSent;

        if (_progress) {
            _progress(totalBytesSent, fileSize, packetsSent);
        }

        if (fileSize == 0) {
            break;
        }
    }

    // Phase 3: Send EOT
    bool eotAcked = false;
    for (int eotRound = 0; eotRound < 10 && !eotAcked; ++eotRound) {
        if (cancelFlag.load()) {
            sendCancel();
            result.errorMessage = "Transfer cancelled by operator";
            return result;
        }

        const char eotByte = static_cast<char>(EOT);
        _tx(&eotByte, 1);

        int resp = _rx(5000);
        if (resp == ACK) {
            eotAcked = true;
            break;
        } else if (resp == CAN) {
            result.errorMessage = "Transfer cancelled by receiver during EOT";
            return result;
        }
    }

    if (!eotAcked) {
        sendCancel();
        result.errorMessage = "Failed EOT handshake with receiver";
        return result;
    }

    auto endTime = std::chrono::steady_clock::now();
    result.elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    if (result.elapsedSec > 0.001) {
        result.transferRateKBps = (result.bytesSent / 1024.0) / result.elapsedSec;
    }
    result.success = true;
    return result;
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
