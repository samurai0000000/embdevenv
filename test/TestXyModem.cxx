/*
 * TestXyModem.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "XyModem.hxx"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

using namespace embdevenv;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "Check failed: " #expr " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        std::abort(); \
    } \
} while (0)

static bool readAll(int fd, uint8_t *buf, size_t len, int timeoutMs = 2000)
{
    size_t total = 0;
    while (total < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) {
            return false;
        }
        ssize_t n = ::read(fd, buf + total, len - total);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

static void testCrc16AndChecksum8()
{
    std::cout << "[TEST] CRC-16 and Checksum-8 verification... ";

    // Test vector: "123456789" -> 0x31C3
    const char *testVec = "123456789";
    uint16_t crc = XyModem::crc16(reinterpret_cast<const uint8_t*>(testVec), 9);
    CHECK(crc == 0x31C3);

    // Empty buffer
    uint16_t emptyCrc = XyModem::crc16(nullptr, 0);
    CHECK(emptyCrc == 0x0000);

    // Checksum 8
    uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t csum = XyModem::checksum8(data, sizeof(data));
    CHECK(csum == 10);

    std::cout << "PASSED\n";
}

static void testYmodemLoopback()
{
    std::cout << "[TEST] YMODEM loopback transfer... ";

    // Create temporary test file (5000 bytes with pseudo-random pattern)
    std::string testPath = "/tmp/embdevenv_test_ymodem.bin";
    std::vector<uint8_t> testData(5000);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    {
        std::ofstream out(testPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::vector<uint8_t> receivedData;
    std::string receivedFilename;
    size_t receivedFilesize = 0;
    bool receiverSuccess = false;

    std::thread receiver([&]() {
        int rfd = sv[1];

        // 1. Emit 'C'
        uint8_t cByte = XyModem::CHAR_C;
        if (::write(rfd, &cByte, 1) != 1) return;

        // 2. Read Packet 0 (133 bytes: SOH, 00, FF, 128 data, 2 CRC)
        uint8_t pkt0[133];
        if (!readAll(rfd, pkt0, sizeof(pkt0))) return;
        if (pkt0[0] != XyModem::SOH || pkt0[1] != 0x00 || pkt0[2] != 0xFF) return;

        uint16_t p0Crc = (static_cast<uint16_t>(pkt0[131]) << 8) | pkt0[132];
        if (XyModem::crc16(pkt0 + 3, 128) != p0Crc) return;

        // Extract filename and size
        const char *payloadStr = reinterpret_cast<const char*>(pkt0 + 3);
        receivedFilename = std::string(payloadStr);
        const char *sizeStr = payloadStr + receivedFilename.size() + 1;
        receivedFilesize = static_cast<size_t>(std::strtoul(sizeStr, nullptr, 10));

        // Send ACK, then 'C'
        uint8_t ackByte = XyModem::ACK;
        if (::write(rfd, &ackByte, 1) != 1) return;
        if (::write(rfd, &cByte, 1) != 1) return;

        // 3. Read data blocks (1029 bytes: STX, seq, ~seq, 1024 data, 2 CRC)
        uint8_t expectedSeq = 1;
        size_t bytesRemaining = receivedFilesize;

        while (bytesRemaining > 0) {
            uint8_t block[1029];
            if (!readAll(rfd, block, sizeof(block))) return;
            if (block[0] != XyModem::STX) return;
            if (block[1] != expectedSeq) return;
            if (block[2] != static_cast<uint8_t>(~expectedSeq)) return;

            uint16_t blockCrc = (static_cast<uint16_t>(block[1027]) << 8) | block[1028];
            if (XyModem::crc16(block + 3, 1024) != blockCrc) return;

            size_t toCopy = std::min(bytesRemaining, static_cast<size_t>(1024));
            receivedData.insert(receivedData.end(), block + 3, block + 3 + toCopy);
            bytesRemaining -= toCopy;
            expectedSeq++;

            // ACK block
            if (::write(rfd, &ackByte, 1) != 1) return;
        }

        // 4. EOT handshake
        uint8_t eotByte = 0;
        if (!readAll(rfd, &eotByte, 1)) return;
        if (eotByte != XyModem::EOT) return;
        uint8_t nakByte = XyModem::NAK;
        if (::write(rfd, &nakByte, 1) != 1) return;

        if (!readAll(rfd, &eotByte, 1)) return;
        if (eotByte != XyModem::EOT) return;
        if (::write(rfd, &ackByte, 1) != 1) return;
        if (::write(rfd, &cByte, 1) != 1) return;

        // 5. Empty Packet 0 (End of batch)
        uint8_t emptyPkt[133];
        if (!readAll(rfd, emptyPkt, sizeof(emptyPkt))) return;
        if (emptyPkt[0] != XyModem::SOH || emptyPkt[1] != 0x00) return;
        if (::write(rfd, &ackByte, 1) != 1) return;

        receiverSuccess = true;
    });

    // Sender setup
    int sfd = sv[0];
    XyModem::TxFunc tx = [sfd](const char *data, size_t len) -> ssize_t {
        return ::write(sfd, data, len);
    };
    XyModem::RxFunc rx = [sfd](int timeoutMs) -> int {
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) return -1;
        uint8_t b;
        ssize_t n = ::read(sfd, &b, 1);
        return (n == 1) ? static_cast<int>(b) : -1;
    };

    std::atomic<bool> cancelFlag{false};
    XyModem modem(tx, rx);
    auto res = modem.send(XyModem::Protocol::YMODEM, testPath, cancelFlag);

    receiver.join();
    ::close(sv[0]);
    ::close(sv[1]);
    ::unlink(testPath.c_str());

    CHECK(res.success);
    CHECK(receiverSuccess);
    CHECK(receivedFilename == "embdevenv_test_ymodem.bin");
    CHECK(receivedFilesize == 5000);
    CHECK(receivedData == testData);

    std::cout << "PASSED (" << res.bytesSent << " bytes sent in "
              << res.packetsSent << " packets)\n";
}

static void testXmodemLoopbackCrc()
{
    std::cout << "[TEST] XMODEM (CRC mode) loopback transfer... ";

    std::string testPath = "/tmp/embdevenv_test_xmodem_crc.bin";
    std::vector<uint8_t> testData(2500);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>((i * 19 + 7) & 0xFF);
    }
    {
        std::ofstream out(testPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::vector<uint8_t> receivedData;
    bool receiverSuccess = false;

    std::thread receiver([&]() {
        int rfd = sv[1];

        // 1. Emit 'C' (requests CRC mode)
        uint8_t cByte = XyModem::CHAR_C;
        if (::write(rfd, &cByte, 1) != 1) return;

        // 2. Read blocks: 2x 1024-byte STX blocks, then 1x 512-byte (as 1024 STX or 128 SOH blocks)
        uint8_t expectedSeq = 1;
        size_t bytesReceived = 0;
        uint8_t ackByte = XyModem::ACK;

        while (bytesReceived < 2500) {
            uint8_t header[3];
            if (!readAll(rfd, header, 3)) return;
            if (header[1] != expectedSeq) return;

            size_t blockSize = (header[0] == XyModem::STX) ? 1024 : 128;
            std::vector<uint8_t> payload(blockSize);
            if (!readAll(rfd, payload.data(), blockSize)) return;

            uint8_t crcBytes[2];
            if (!readAll(rfd, crcBytes, 2)) return;
            uint16_t pktCrc = (static_cast<uint16_t>(crcBytes[0]) << 8) | crcBytes[1];
            if (XyModem::crc16(payload.data(), blockSize) != pktCrc) return;

            size_t toCopy = std::min(static_cast<size_t>(2500 - bytesReceived), blockSize);
            receivedData.insert(receivedData.end(), payload.begin(), payload.begin() + toCopy);
            bytesReceived += toCopy;
            expectedSeq++;

            if (::write(rfd, &ackByte, 1) != 1) return;
        }

        // 3. EOT
        uint8_t eotByte = 0;
        if (!readAll(rfd, &eotByte, 1)) return;
        if (eotByte != XyModem::EOT) return;
        if (::write(rfd, &ackByte, 1) != 1) return;

        receiverSuccess = true;
    });

    int sfd = sv[0];
    XyModem::TxFunc tx = [sfd](const char *data, size_t len) -> ssize_t {
        return ::write(sfd, data, len);
    };
    XyModem::RxFunc rx = [sfd](int timeoutMs) -> int {
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) return -1;
        uint8_t b;
        ssize_t n = ::read(sfd, &b, 1);
        return (n == 1) ? static_cast<int>(b) : -1;
    };

    std::atomic<bool> cancelFlag{false};
    XyModem modem(tx, rx);
    auto res = modem.send(XyModem::Protocol::XMODEM, testPath, cancelFlag);

    receiver.join();
    ::close(sv[0]);
    ::close(sv[1]);
    ::unlink(testPath.c_str());

    CHECK(res.success);
    CHECK(receiverSuccess);
    CHECK(receivedData == testData);

    std::cout << "PASSED (" << res.bytesSent << " bytes sent in "
              << res.packetsSent << " packets)\n";
}

static void testCancellation()
{
    std::cout << "[TEST] Transfer cancellation handling... ";

    std::string testPath = "/tmp/embdevenv_test_cancel.bin";
    std::vector<uint8_t> testData(20000, 0xAA);
    {
        std::ofstream out(testPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::atomic<bool> cancelFlag{false};
    bool sawCanSequence = false;

    std::thread receiver([&]() {
        int rfd = sv[1];

        // Emit 'C'
        uint8_t cByte = XyModem::CHAR_C;
        if (::write(rfd, &cByte, 1) != 1) return;

        // Read Packet 0
        uint8_t pkt0[133];
        if (!readAll(rfd, pkt0, sizeof(pkt0))) return;
        uint8_t ackByte = XyModem::ACK;
        if (::write(rfd, &ackByte, 1) != 1) return;
        if (::write(rfd, &cByte, 1) != 1) return;

        // Read first data packet
        uint8_t block[1029];
        if (!readAll(rfd, block, sizeof(block))) return;

        // Trigger cancellation from operator side now!
        cancelFlag.store(true);

        // Send ACK for packet 1
        if (::write(rfd, &ackByte, 1) != 1) return;

        // Sender should now abort and send CAN sequence (0x18 ...)
        uint8_t abortBuf[16];
        if (readAll(rfd, abortBuf, 5, 2000)) {
            if (abortBuf[0] == XyModem::CAN && abortBuf[1] == XyModem::CAN) {
                sawCanSequence = true;
            }
        }
    });

    int sfd = sv[0];
    XyModem::TxFunc tx = [sfd](const char *data, size_t len) -> ssize_t {
        return ::write(sfd, data, len);
    };
    XyModem::RxFunc rx = [sfd](int timeoutMs) -> int {
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) return -1;
        uint8_t b;
        ssize_t n = ::read(sfd, &b, 1);
        return (n == 1) ? static_cast<int>(b) : -1;
    };

    XyModem modem(tx, rx);
    auto res = modem.send(XyModem::Protocol::YMODEM, testPath, cancelFlag);

    receiver.join();
    ::close(sv[0]);
    ::close(sv[1]);
    ::unlink(testPath.c_str());

    CHECK(!res.success);
    CHECK(sawCanSequence);
    CHECK(res.errorMessage.find("cancelled") != std::string::npos);

    std::cout << "PASSED\n";
}

static void testXmodemLoopbackChecksum()
{
    std::cout << "[TEST] XMODEM (Checksum mode) loopback transfer... ";

    std::string testPath = "/tmp/embdevenv_test_xmodem_csum.bin";
    std::vector<uint8_t> testData(1500);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);
    }
    {
        std::ofstream out(testPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::vector<uint8_t> receivedData;
    bool receiverSuccess = false;

    std::thread receiver([&]() {
        int rfd = sv[1];

        // 1. Emit NAK (requests legacy Checksum mode)
        uint8_t nakByte = XyModem::NAK;
        if (::write(rfd, &nakByte, 1) != 1) return;

        // 2. Read blocks: 1x 1024 STX block, then 4x 128 SOH blocks (or 1x 1024 STX block)
        uint8_t expectedSeq = 1;
        size_t bytesReceived = 0;
        uint8_t ackByte = XyModem::ACK;

        while (bytesReceived < 1500) {
            uint8_t header[3];
            if (!readAll(rfd, header, 3)) return;
            if (header[1] != expectedSeq) return;

            size_t blockSize = (header[0] == XyModem::STX) ? 1024 : 128;
            std::vector<uint8_t> payload(blockSize);
            if (!readAll(rfd, payload.data(), blockSize)) return;

            uint8_t csumByte = 0;
            if (!readAll(rfd, &csumByte, 1)) return;
            if (XyModem::checksum8(payload.data(), blockSize) != csumByte) return;

            size_t toCopy = std::min(static_cast<size_t>(1500 - bytesReceived), blockSize);
            receivedData.insert(receivedData.end(), payload.begin(), payload.begin() + toCopy);
            bytesReceived += toCopy;
            expectedSeq++;

            if (::write(rfd, &ackByte, 1) != 1) return;
        }

        // 3. EOT
        uint8_t eotByte = 0;
        if (!readAll(rfd, &eotByte, 1)) return;
        if (eotByte != XyModem::EOT) return;
        if (::write(rfd, &ackByte, 1) != 1) return;

        receiverSuccess = true;
    });

    int sfd = sv[0];
    XyModem::TxFunc tx = [sfd](const char *data, size_t len) -> ssize_t {
        return ::write(sfd, data, len);
    };
    XyModem::RxFunc rx = [sfd](int timeoutMs) -> int {
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0) return -1;
        uint8_t b;
        ssize_t n = ::read(sfd, &b, 1);
        return (n == 1) ? static_cast<int>(b) : -1;
    };

    std::atomic<bool> cancelFlag{false};
    XyModem modem(tx, rx);
    auto res = modem.send(XyModem::Protocol::XMODEM, testPath, cancelFlag);

    receiver.join();
    ::close(sv[0]);
    ::close(sv[1]);
    ::unlink(testPath.c_str());

    CHECK(res.success);
    CHECK(receiverSuccess);
    CHECK(receivedData == testData);

    std::cout << "PASSED (" << res.bytesSent << " bytes sent in "
              << res.packetsSent << " packets)\n";
}

int main()
{
    std::cout << "=== Running XyModem Protocol Unit Tests ===\n";
    testCrc16AndChecksum8();
    testYmodemLoopback();
    testXmodemLoopbackCrc();
    testXmodemLoopbackChecksum();
    testCancellation();
    std::cout << "=== All XyModem Tests Passed Successfully ===\n";
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
