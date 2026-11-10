/*
 * TestMcpStreamableHttp.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "StandaloneMcpServer.hxx"
#include "AimonGatewayClient.hxx"
#include "TargetManager.hxx"
#include "Target.hxx"
#include "Console.hxx"
#include "ToolDispatcher.hxx"
#include "Version.hxx"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

using namespace embdevenv;
using json = nlohmann::json;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            g_testsFailed++; \
        } else { \
            g_testsPassed++; \
        } \
    } while (0)

static void testWebServerStreamableHttp()
{
    std::cout << "[TEST] testWebServerStreamableHttp..." << std::endl;

    WebServer server(0, "127.0.0.1", false);
    TEST_ASSERT(server.start(), "server.start() must succeed");

    int port = server.getPort();
    TEST_ASSERT(port > 0, "server.getPort() must return kernel-assigned port > 0");

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);

    // 1. OPTIONS /mcp CORS test
    auto optRes = client.Options("/mcp");
    TEST_ASSERT(optRes != nullptr && optRes->status == 204, "OPTIONS /mcp returns 204");
    if (optRes) {
        TEST_ASSERT(optRes->get_header_value("Access-Control-Allow-Origin") == "*",
                    "CORS Access-Control-Allow-Origin is *");
        TEST_ASSERT(optRes->has_header("Access-Control-Allow-Methods"),
                    "CORS Access-Control-Allow-Methods is present");
        TEST_ASSERT(optRes->has_header("Access-Control-Expose-Headers"),
                    "CORS Access-Control-Expose-Headers is present");
    }

    // 2. Malformed JSON returns 400 with -32700
    auto badRes = client.Post("/mcp", "invalid json", "application/json");
    TEST_ASSERT(badRes != nullptr && badRes->status == 400, "Malformed JSON returns 400");
    if (badRes) {
        auto jBad = json::parse(badRes->body);
        TEST_ASSERT(jBad["error"]["code"] == -32700, "Error code is -32700 for parse error");
    }

    // 3. POST /mcp initialize negotiating 2025-03-26 and receiving Mcp-Session-Id
    json initPayload = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"}
        }}
    };

    auto initRes = client.Post("/mcp", initPayload.dump(), "application/json");
    TEST_ASSERT(initRes != nullptr && initRes->status == 200, "initialize returns 200");
    std::string sessionId;
    if (initRes) {
        TEST_ASSERT(initRes->has_header("Mcp-Session-Id"), "Mcp-Session-Id header present");
        sessionId = initRes->get_header_value("Mcp-Session-Id");
        TEST_ASSERT(!sessionId.empty(), "Mcp-Session-Id is non-empty");

        auto jInit = json::parse(initRes->body);
        TEST_ASSERT(jInit["result"]["protocolVersion"] == "2025-03-26",
                    "protocolVersion negotiated to 2025-03-26");
        TEST_ASSERT(jInit["result"]["serverInfo"]["name"] == "embdevenv",
                    "serverInfo.name is embdevenv");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("tools"),
                    "capabilities has tools");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("resources"),
                    "capabilities has resources");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("prompts"),
                    "capabilities has prompts");
    }

    httplib::Headers sessionHeaders = {
        {"Mcp-Session-Id", sessionId}
    };

    // 4. POST /mcp notifications/initialized returns HTTP 202 Accepted and empty body
    json notifPayload = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"}
    };
    auto notifRes = client.Post("/mcp", sessionHeaders, notifPayload.dump(), "application/json");
    TEST_ASSERT(notifRes != nullptr && notifRes->status == 202,
                "notifications/initialized returns 202 Accepted");
    if (notifRes) {
        TEST_ASSERT(notifRes->body.empty(), "notifications/initialized body is empty");
    }

    // 5. POST /mcp tools/list
    json toolsPayload = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/list"}
    };
    auto toolsRes = client.Post("/mcp", sessionHeaders, toolsPayload.dump(), "application/json");
    TEST_ASSERT(toolsRes != nullptr && toolsRes->status == 200, "tools/list returns 200");
    if (toolsRes) {
        auto jTools = json::parse(toolsRes->body);
        TEST_ASSERT(jTools["result"]["tools"].is_array(), "tools is an array");
        TEST_ASSERT(!jTools["result"]["tools"].empty(), "tools has entries");
    }

    // 6. POST /mcp resources/list
    json resPayload = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "resources/list"}
    };
    auto resRes = client.Post("/mcp", sessionHeaders, resPayload.dump(), "application/json");
    TEST_ASSERT(resRes != nullptr && resRes->status == 200, "resources/list returns 200");
    if (resRes) {
        auto jRes = json::parse(resRes->body);
        TEST_ASSERT(jRes["result"]["resources"].is_array(), "resources is an array");
    }

    // 7. POST /mcp prompts/list
    json promptsPayload = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "prompts/list"}
    };
    auto promptsRes = client.Post("/mcp", sessionHeaders, promptsPayload.dump(), "application/json");
    TEST_ASSERT(promptsRes != nullptr && promptsRes->status == 200, "prompts/list returns 200");
    if (promptsRes) {
        auto jPrompts = json::parse(promptsRes->body);
        TEST_ASSERT(jPrompts["result"]["prompts"].is_array(), "prompts is an array");
    }

    // 8. POST /mcp tools/call (embdevenv_list_targets)
    json callPayload = {
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"method", "tools/call"},
        {"params", {
            {"name", "embdevenv_list_targets"},
            {"arguments", json::object()}
        }}
    };
    auto callRes = client.Post("/mcp", sessionHeaders, callPayload.dump(), "application/json");
    TEST_ASSERT(callRes != nullptr && callRes->status == 200, "tools/call returns 200");
    if (callRes) {
        auto jCall = json::parse(callRes->body);
        TEST_ASSERT(jCall["result"]["content"].is_array(), "call content is array");
        TEST_ASSERT(jCall["result"]["content"][0]["type"] == "text", "content[0].type is text");
    }

    // 9. POST /mcp JSON-RPC batch array
    json batchPayload = json::array({
        {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "prompts/list"}},
        {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}
    });
    auto batchRes = client.Post("/mcp", sessionHeaders, batchPayload.dump(), "application/json");
    TEST_ASSERT(batchRes != nullptr && batchRes->status == 200, "batch array returns 200");
    if (batchRes) {
        auto jBatch = json::parse(batchRes->body);
        TEST_ASSERT(jBatch.is_array(), "batch response is an array");
        TEST_ASSERT(jBatch.size() == 2, "batch response has 2 elements (notification omitted)");
        TEST_ASSERT(jBatch[0]["id"] == 10, "batch[0] id is 10");
        TEST_ASSERT(jBatch[1]["id"] == 11, "batch[1] id is 11");
    }

    // 10. GET /mcp with Accept: text/event-stream
    httplib::Headers sseHeaders = {
        {"Accept", "text/event-stream"},
        {"Mcp-Session-Id", sessionId}
    };
    int getStatus = 0;
    std::string contentType;
    std::string receivedChunk;
    client.Get("/mcp", sseHeaders,
        [&](const httplib::Response &response) {
            getStatus = response.status;
            contentType = response.get_header_value("Content-Type");
            return true;
        },
        [&](const char *data, size_t data_length) {
            receivedChunk.append(data, data_length);
            return false; // Stop after receiving initial connected frame
        });

    TEST_ASSERT(getStatus == 200, "GET /mcp status is 200");
    TEST_ASSERT(contentType.find("text/event-stream") != std::string::npos,
                "Content-Type is text/event-stream");
    TEST_ASSERT(receivedChunk.find(": connected") != std::string::npos,
                "Received : connected comment");
    TEST_ASSERT(receivedChunk.find("event: endpoint") == std::string::npos,
                "Stream never emits event: endpoint");

    // 11. DELETE /mcp returns 204 No Content
    auto delRes = client.Delete("/mcp", sessionHeaders);
    TEST_ASSERT(delRes != nullptr && delRes->status == 204, "DELETE /mcp returns 204");

    // 12. Legacy GET /sse and POST /message backward compatibility
    auto legacySseRes = client.Get("/sse");
    TEST_ASSERT(legacySseRes != nullptr && legacySseRes->status == 200, "GET /sse returns 200");
    if (legacySseRes) {
        TEST_ASSERT(legacySseRes->body.find("event: endpoint") != std::string::npos,
                    "legacy /sse returns event: endpoint");
    }

    json legMsgPayload = {
        {"jsonrpc", "2.0"},
        {"id", 20},
        {"method", "initialize"}
    };
    auto legMsgRes = client.Post("/message", legMsgPayload.dump(), "application/json");
    TEST_ASSERT(legMsgRes != nullptr && legMsgRes->status == 200, "POST /message returns 200");
    if (legMsgRes) {
        auto jLeg = json::parse(legMsgRes->body);
        TEST_ASSERT(jLeg["result"]["protocolVersion"] == "2024-11-05",
                    "legacy /message defaults to 2024-11-05");
    }

    server.stop();
}

static void testStandaloneMcpServer()
{
    std::cout << "[TEST] testStandaloneMcpServer..." << std::endl;

    StandaloneMcpServer mcpServer(0);
    TEST_ASSERT(mcpServer.start(), "StandaloneMcpServer::start() on port 0 succeeds");

    int port = mcpServer.getPort();
    TEST_ASSERT(port > 0, "StandaloneMcpServer::getPort() returns bound port > 0");

    int sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(sockFd >= 0, "socket() created successfully");

    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &servAddr.sin_addr);

    int connRet = ::connect(sockFd, reinterpret_cast<struct sockaddr *>(&servAddr), sizeof(servAddr));
    TEST_ASSERT(connRet == 0, "connect() to StandaloneMcpServer succeeded");

    auto sendLine = [sockFd](const std::string &line) {
        std::string s = line + "\n";
        ::send(sockFd, s.c_str(), s.size(), 0);
    };

    auto readLine = [sockFd](int timeoutMs = 2000) -> std::string {
        std::string result;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < timeoutMs) {
            struct pollfd pfd;
            pfd.fd = sockFd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int ret = poll(&pfd, 1, 100);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                char ch;
                ssize_t n = ::recv(sockFd, &ch, 1, 0);
                if (n > 0) {
                    if (ch == '\n') break;
                    if (ch != '\r') result.push_back(ch);
                } else {
                    break;
                }
            }
        }
        return result;
    };

    // 1. initialize negotiating 2025-03-26
    sendLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    std::string line = readLine();
    TEST_ASSERT(!line.empty(), "Received initialize response from StandaloneMcpServer");
    if (!line.empty()) {
        auto jInit = json::parse(line);
        TEST_ASSERT(jInit["result"]["protocolVersion"] == "2025-03-26",
                    "StandaloneMcpServer negotiated 2025-03-26");
        TEST_ASSERT(jInit["result"]["serverInfo"]["name"] == "embdevenv",
                    "serverInfo.name is embdevenv");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("tools"),
                    "capabilities has tools");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("resources"),
                    "capabilities has resources");
        TEST_ASSERT(jInit["result"]["capabilities"].contains("prompts"),
                    "capabilities has prompts");
    }

    // 2. notifications/initialized produces no response frame
    sendLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    std::string noResp = readLine(200);
    TEST_ASSERT(noResp.empty(), "notifications/initialized produced no response frame");

    // 3. ping
    sendLine(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    std::string pingResp = readLine();
    TEST_ASSERT(!pingResp.empty(), "Received ping response");
    if (!pingResp.empty()) {
        auto jPing = json::parse(pingResp);
        TEST_ASSERT(jPing["id"] == 2 && jPing.contains("result"), "ping result ok");
    }

    // 4. resources/list
    sendLine(R"({"jsonrpc":"2.0","id":3,"method":"resources/list"})");
    std::string resResp = readLine();
    TEST_ASSERT(!resResp.empty(), "Received resources/list response");
    if (!resResp.empty()) {
        auto jRes = json::parse(resResp);
        TEST_ASSERT(jRes["result"]["resources"].is_array(), "resources/list returned array");
    }

    // 5. prompts/list
    sendLine(R"({"jsonrpc":"2.0","id":4,"method":"prompts/list"})");
    std::string promptsResp = readLine();
    TEST_ASSERT(!promptsResp.empty(), "Received prompts/list response");
    if (!promptsResp.empty()) {
        auto jPrompts = json::parse(promptsResp);
        TEST_ASSERT(jPrompts["result"]["prompts"].is_array(), "prompts/list returned array");
    }

    // 6. tools/list
    sendLine(R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    std::string toolsResp = readLine();
    TEST_ASSERT(!toolsResp.empty(), "Received tools/list response");
    if (!toolsResp.empty()) {
        auto jTools = json::parse(toolsResp);
        TEST_ASSERT(jTools["result"]["tools"].is_array(), "tools/list returned array");
        TEST_ASSERT(!jTools["result"]["tools"].empty(), "tools array has tools");
    }

    ::close(sockFd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mcpServer.stop();
}

static void testAimonGatewayConcurrentAndTimeoutMs()
{
    std::cout << "[TEST] testAimonGatewayConcurrentAndTimeoutMs..." << std::endl;

    // 1. Verify schema exposes timeout_ms on embdevenv_console_expect
    json tools = ToolDispatcher::getToolDefinitions();
    bool foundExpectTimeoutMs = false;
    for (const auto &t : tools) {
        if (t.value("name", "") == "embdevenv_console_expect") {
            const auto &props = t["inputSchema"]["properties"];
            if (props.contains("timeout_ms")) {
                foundExpectTimeoutMs = true;
            }
        }
    }
    TEST_ASSERT(foundExpectTimeoutMs, "embdevenv_console_expect schema includes timeout_ms");

    // 2. Register a synthetic target with an unbacked console so expect waits for timeout_ms
    auto target = std::make_shared<Target>("mock-devkit", "n1-655-devkit", "Mock Devkit", true, 0);
    auto console2 = std::make_shared<Console>("console2", "/dev/null", 115200, 100, 0);
    target->addConsole(console2);
    TargetManager::getInstance().addTarget(target);

    // 3. Spin up a local mock aimon TCP gateway listener on an ephemeral port
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listenFd >= 0, "mock gateway listen socket created");
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT(::bind(listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0,
                "mock gateway bind succeeded");

    socklen_t addrLen = sizeof(addr);
    TEST_ASSERT(::getsockname(listenFd, reinterpret_cast<struct sockaddr *>(&addr), &addrLen) == 0,
                "getsockname succeeded");
    int gwPort = ntohs(addr.sin_port);
    TEST_ASSERT(::listen(listenFd, 1) == 0, "mock gateway listen succeeded");

    auto &gwClient = AimonGatewayClient::getInstance();
    gwClient.start("127.0.0.1", gwPort);

    struct pollfd acceptPfd{listenFd, POLLIN, 0};
    TEST_ASSERT(poll(&acceptPfd, 1, 3000) > 0, "AimonGatewayClient connected to mock gateway");
    int connFd = ::accept(listenFd, nullptr, nullptr);
    TEST_ASSERT(connFd >= 0, "accept() succeeded");

    std::string recvBuf;
    auto readJsonLine = [&](int timeoutMs) -> json {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < timeoutMs) {
            size_t nl = recvBuf.find('\n');
            if (nl != std::string::npos) {
                std::string line = recvBuf.substr(0, nl);
                recvBuf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) return json::parse(line);
            }
            struct pollfd pfd{connFd, POLLIN, 0};
            if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
                char buf[1024];
                ssize_t n = ::recv(connFd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                recvBuf.append(buf, static_cast<size_t>(n));
            }
        }
        return json();
    };

    auto regJson = readJsonLine(2000);
    TEST_ASSERT(!regJson.is_null() && regJson.value("method", "") == "gateway/register",
                "Received gateway/register from AimonGatewayClient");

    // Issue a 500ms blocking embdevenv_console_expect (using timeout_ms) on console2,
    // and immediately issue a fast embdevenv_list_targets call behind it on the same socket.
    auto t0 = std::chrono::steady_clock::now();
    json expectReq = {
        {"jsonrpc", "2.0"},
        {"id", "req-expect"},
        {"method", "tools/call"},
        {"params", {
            {"name", "embdevenv_console_expect"},
            {"arguments", {
                {"target", "mock-devkit"},
                {"console", "console2"},
                {"wait_for", json::array({"r52-cli>"})},
                {"timeout_ms", 500}
            }}
        }}
    };
    json listReq = {
        {"jsonrpc", "2.0"},
        {"id", "req-list"},
        {"method", "tools/call"},
        {"params", {
            {"name", "embdevenv_list_targets"},
            {"arguments", json::object()}
        }}
    };
    std::string wire = expectReq.dump() + "\n" + listReq.dump() + "\n";
    ::send(connFd, wire.data(), wire.size(), 0);

    // First response must be req-list (completing while req-expect is still waiting!)
    json firstResp = readJsonLine(1500);
    auto tFirstMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT(!firstResp.is_null() && firstResp.value("id", "") == "req-list",
                "Fast tools/call completed out-of-order without head-of-line blocking");
    TEST_ASSERT(tFirstMs < 350, "Fast tools/call completed before 500ms expect finished");

    // Second response must be req-expect honoring timeout_ms = 500 (not 60s!)
    json secondResp = readJsonLine(2000);
    auto tSecondMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT(!secondResp.is_null() && secondResp.value("id", "") == "req-expect",
                "Received expect response after timeout_ms");
    TEST_ASSERT(tSecondMs >= 450 && tSecondMs < 1500,
                "embdevenv_console_expect honored timeout_ms=500 instead of 60s default");

    gwClient.stop();
    gwClient.join();
    ::close(connFd);
    ::close(listenFd);
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << " Running McpStreamableHttp Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testWebServerStreamableHttp();
    testStandaloneMcpServer();
    testAimonGatewayConcurrentAndTimeoutMs();

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " Tests Passed: " << g_testsPassed << std::endl;
    std::cout << " Tests Failed: " << g_testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return g_testsFailed == 0 ? 0 : 1;
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
