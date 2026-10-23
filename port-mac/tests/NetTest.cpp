// Stage E5 test for the macOS network layer, without a WoW server.
//
//   NetTest
//
// Starts a TCP server on 127.0.0.1 in a thread of this process and drives WowConnection the way Grunt does
// (InitOsNet, new WowConnection(response), SetType, Connect(host, port), Send(data, size, false), callbacks on the
// network worker threads):
//  1. stream echo: connect by IP, send small and large (2 MB, queued through DoWrites) buffers, receive them back
//     through WCDataReady, then the server closes → WCDisconnected.
//  2. host name: connect to "localhost" (getaddrinfo), client-side Disconnect() → WCDisconnected.
//  3. message mode: the server sends framed packets (2- and 3-byte headers) → WCMessageReady.
//  4. refused: connect to a closed port → WCCantConnect.
//  5. immediate failure: connect() itself fails (a multicast address; on a user's machine, EHOSTUNREACH without
//     Local Network access) → WCCantConnect, instead of a login stuck on "Connecting".
// Also checks the OsNetAddr helpers and the Mac versionproof checksum (IOsVersionChecksum).
// Exit code 0 when everything passed.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/WowServices/WowConnection.h"

static int32_t s_failures;

// WowConnectionNet::Stop logs through the game console (WoW/Source/Console, which needs the whole UI); print instead.
void ConsolePrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        s_failures++;
    }
}

// Records every callback; the test thread waits on it.
class TestResponse : public WowConnectionResponse {
    public:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    int32_t m_connected = 0;
    int32_t m_cantConnect = 0;
    int32_t m_disconnected = 0;
    std::vector<uint8_t> m_data;
    std::vector<std::string> m_messages;
    NETCONNADDR m_addr = {};

    void WCMessageReady(WowConnection* connection, uint32_t timeStamp, CDataStore* msg) override {
        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_messages.emplace_back(reinterpret_cast<const char*>(msg->m_data), msg->m_size);
        this->m_cond.notify_all();
    }

    void WCConnected(WowConnection* connection, WowConnection* inbound, uint32_t timeStamp, const NETCONNADDR* addr) override {
        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_connected++;
        this->m_addr = *addr;
        this->m_cond.notify_all();
    }

    void WCCantConnect(WowConnection* connection, uint32_t timeStamp, const NETCONNADDR* addr) override {
        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_cantConnect++;
        this->m_cond.notify_all();
    }

    void WCDisconnected(WowConnection* connection, uint32_t timeStamp, const NETCONNADDR* addr) override {
        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_disconnected++;
        this->m_cond.notify_all();
    }

    void WCDataReady(WowConnection* connection, uint32_t timeStamp, uint8_t* data, int32_t length) override {
        std::lock_guard<std::mutex> lock(this->m_mutex);
        this->m_data.insert(this->m_data.end(), data, data + length);
        this->m_cond.notify_all();
    }

    bool Wait(const std::function<bool()>& done, int32_t ms = 5000) {
        std::unique_lock<std::mutex> lock(this->m_mutex);
        return this->m_cond.wait_for(lock, std::chrono::milliseconds(ms), done);
    }
};

// One-shot TCP server on 127.0.0.1: accepts a single connection and runs `handler` on it (blocking socket).
class TestServer {
    public:
    int32_t m_listen = -1;
    uint16_t m_port = 0;
    std::thread m_thread;

    explicit TestServer(std::function<void(int32_t)> handler) {
        this->m_listen = socket(AF_INET, SOCK_STREAM, 0);
        int32_t reuse = 1;
        setsockopt(this->m_listen, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address = {};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(this->m_listen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        listen(this->m_listen, 4);

        socklen_t length = sizeof(address);
        getsockname(this->m_listen, reinterpret_cast<sockaddr*>(&address), &length);
        this->m_port = ntohs(address.sin_port);

        int32_t listenSock = this->m_listen;
        this->m_thread = std::thread([listenSock, handler]() {
            int32_t sock = accept(listenSock, nullptr, nullptr);
            if (sock >= 0) {
                int32_t noSigPipe = 1;
                setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
                handler(sock);
                close(sock);
            }
        });
    }

    ~TestServer() {
        shutdown(this->m_listen, SHUT_RDWR);
        close(this->m_listen);
        if (this->m_thread.joinable()) {
            this->m_thread.join();
        }
    }
};

// Echoes until `total` bytes went through (or the peer closes), then closes.
static void EchoHandler(int32_t sock, size_t total) {
    std::vector<uint8_t> buffer(65536);
    size_t echoed = 0;

    while (echoed < total) {
        ssize_t bytes = recv(sock, buffer.data(), buffer.size(), 0);
        if (bytes <= 0) {
            return;
        }

        for (ssize_t sent = 0; sent < bytes;) {
            ssize_t n = send(sock, buffer.data() + sent, bytes - sent, 0);
            if (n <= 0) {
                return;
            }
            sent += n;
        }

        echoed += bytes;
    }
}

static void TestStreamEcho() {
    std::printf("-- stream echo (connect by IP, server closes)\n");

    std::vector<uint8_t> small = { 'h', 'e', 'l', 'l', 'o', ' ', 'g', 'r', 'u', 'n', 't' };
    std::vector<uint8_t> large(2 * 1024 * 1024);
    for (size_t i = 0; i < large.size(); i++) {
        large[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    }

    size_t total = small.size() + large.size();
    TestServer server([total](int32_t sock) { EchoHandler(sock, total); });

    TestResponse response;
    WowConnection* connection = new (__FILE__, __LINE__) WowConnection(&response, nullptr);
    connection->SetType(WOWC_TYPE_STREAM);
    connection->Connect("127.0.0.1", server.m_port, 5000);

    Check(response.Wait([&] { return response.m_connected == 1; }), "WCConnected");
    Check(connection->m_connState == WOWC_CONNECTED, "state is WOWC_CONNECTED");

    uint16_t peerPort = 0;
    uint32_t peerAddress = OsNetAddrGetAddress(&response.m_addr.peerAddr, &peerPort);
    Check(peerAddress == htonl(INADDR_LOOPBACK) && peerPort == server.m_port, "peer address is 127.0.0.1:<server port>");
    Check(OsNetAddrGetAddress(&response.m_addr.selfAddr, nullptr) == htonl(INADDR_LOOPBACK), "self address is 127.0.0.1");

    char text[80];
    connection->PeerAddrToStr(text, sizeof(text));
    std::printf("     peer %s\n", text);

    WC_SEND_RESULT result = connection->Send(small.data(), static_cast<int32_t>(small.size()), false);
    Check(result == WC_SEND_SENT, "small Send → WC_SEND_SENT");
    Check(response.Wait([&] { return response.m_data.size() >= small.size(); }), "small echo received");
    Check(response.m_data == small, "small echo matches");

    result = connection->Send(large.data(), static_cast<int32_t>(large.size()), false);
    std::printf("     large Send → %d (0 sent, 1 queued)\n", result);
    Check(result == WC_SEND_SENT || result == WC_SEND_QUEUED, "large Send accepted");
    Check(response.Wait([&] { return response.m_data.size() >= total; }, 15000), "large echo received");
    Check(response.m_data.size() == total && memcmp(response.m_data.data() + small.size(), large.data(), large.size()) == 0, "large echo matches");

    Check(response.Wait([&] { return response.m_disconnected == 1; }), "WCDisconnected after the server closed");
    Check(connection->m_connState == WOWC_DISCONNECTED && connection->m_sock == -1, "state is WOWC_DISCONNECTED, socket closed");

    connection->SetResponse(nullptr, false);
    connection->Release();
}

static void TestHostNameAndClientDisconnect() {
    std::printf("-- host name + client Disconnect()\n");

    TestServer server([](int32_t sock) { EchoHandler(sock, SIZE_MAX); });

    TestResponse response;
    WowConnection* connection = new (__FILE__, __LINE__) WowConnection(&response, nullptr);
    connection->SetType(WOWC_TYPE_STREAM);
    connection->Connect("localhost", server.m_port, 5000);

    Check(response.Wait([&] { return response.m_connected == 1; }), "WCConnected to localhost");

    uint8_t ping[] = { 1, 2, 3, 4 };
    connection->Send(ping, sizeof(ping), false);
    Check(response.Wait([&] { return response.m_data.size() == sizeof(ping); }), "echo received");

    connection->Disconnect();
    Check(response.Wait([&] { return response.m_disconnected == 1; }), "WCDisconnected after Disconnect()");
    Check(connection->m_connState == WOWC_DISCONNECTED, "state is WOWC_DISCONNECTED");

    connection->SetResponse(nullptr, false);
    connection->Release();
}

static void TestMessages() {
    std::printf("-- message mode (framed packets)\n");

    TestServer server([](int32_t sock) {
        // 2-byte header (big-endian size) and 3-byte header (high bit set, 23-bit size), sent byte by byte to force
        // partial reads, then a final packet; the client closes.
        std::vector<uint8_t> stream = { 0x00, 0x03, 'a', 'b', 'c' };
        std::vector<uint8_t> big(40000, 'x');
        stream.push_back(0x80 | ((big.size() >> 16) & 0x7F));
        stream.push_back((big.size() >> 8) & 0xFF);
        stream.push_back(big.size() & 0xFF);
        stream.insert(stream.end(), big.begin(), big.end());
        const uint8_t last[] = { 0x00, 0x02, 'o', 'k' };

        for (size_t i = 0; i < 5; i++) {
            send(sock, &stream[i], 1, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        send(sock, stream.data() + 5, stream.size() - 5, 0);
        send(sock, last, sizeof(last), 0);

        uint8_t byte;
        recv(sock, &byte, 1, 0);
    });

    TestResponse response;
    WowConnection* connection = new (__FILE__, __LINE__) WowConnection(&response, nullptr);
    connection->SetType(WOWC_TYPE_MESSAGES);
    connection->Connect("127.0.0.1", server.m_port, 5000);

    Check(response.Wait([&] { return response.m_connected == 1; }), "WCConnected");
    Check(response.Wait([&] { return response.m_messages.size() == 3; }), "3 WCMessageReady");

    if (response.m_messages.size() == 3) {
        Check(response.m_messages[0] == "abc", "message 1 (2-byte header)");
        Check(response.m_messages[1] == std::string(40000, 'x'), "message 2 (3-byte header, 40000 bytes)");
        Check(response.m_messages[2] == "ok", "message 3");
    }

    connection->Disconnect();
    Check(response.Wait([&] { return response.m_disconnected == 1; }), "WCDisconnected");

    connection->SetResponse(nullptr, false);
    connection->Release();
}

static void TestRefused() {
    std::printf("-- connect to a closed port\n");

    // Grab a free port and close it again.
    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address = {};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length);
    uint16_t port = ntohs(address.sin_port);
    close(sock);

    TestResponse response;
    WowConnection* connection = new (__FILE__, __LINE__) WowConnection(&response, nullptr);
    connection->SetType(WOWC_TYPE_STREAM);
    connection->Connect("127.0.0.1", port, 5000);

    Check(response.Wait([&] { return response.m_cantConnect == 1; }), "WCCantConnect");
    Check(response.m_connected == 0 && response.m_disconnected == 0, "no WCConnected/WCDisconnected");
    Check(connection->m_connState == WOWC_DISCONNECTED && connection->m_sock == -1, "state is WOWC_DISCONNECTED, socket closed");

    connection->SetResponse(nullptr, false);
    connection->Release();
}

static void TestNetAddr() {
    std::printf("-- OsNetAddr helpers\n");

    NETADDR addr;
    OsNetAddrMake(inet_addr("10.1.2.3"), 3724, &addr);
    uint16_t port = 0;
    Check(OsNetAddrGetAddress(&addr, &port) == inet_addr("10.1.2.3") && port == 3724, "OsNetAddrMake/GetAddress round trip");

    char text[64];
    OsNetAddrToStr(&addr, text, sizeof(text));
    Check(!strcmp(text, "10.1.2.3:3724"), "OsNetAddrToStr");

    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(&addr);
    Check(in->sin_len == sizeof(sockaddr_in) && in->sin_family == AF_INET, "NETADDR is a BSD sockaddr_in");
}

class HashCallback : public IOsVersionHashCallback {
    public:
    uint64_t m_bytes = 0;
    uint64_t m_hash = 1469598103934665603ull;

    void Process(const void* data, uint32_t size) override {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < size; i++) {
            this->m_hash = (this->m_hash ^ bytes[i]) * 1099511628211ull;
        }
        this->m_bytes += size;
    }
};

static void TestVersionChecksum() {
    std::printf("-- IOsVersionChecksum\n");

    HashCallback first;
    HashCallback second;
    Check(IOsVersionChecksum(first, nullptr, 1) == 1 && first.m_bytes > 0, "main executable hashed");
    IOsVersionChecksum(second, nullptr, 1);
    Check(first.m_hash == second.m_hash && first.m_bytes == second.m_bytes, "deterministic");
    std::printf("     %llu bytes of __TEXT\n", static_cast<unsigned long long>(first.m_bytes));

    HashCallback none;
    Check(IOsVersionChecksum(none, "DivxDecoder.dll", 1) == 0 && none.m_bytes == 0, "DivxDecoder.dll not loaded → 0");
    Check(IOsVersionChecksum(none, "unicows.dll", 0) == 0 && none.m_bytes == 0, "unicows.dll missing → 0");
}

static void TestImmediateFailure() {
    std::printf("-- connect() fails at once\n");

    TestResponse response;
    WowConnection* connection = new (__FILE__, __LINE__) WowConnection(&response, nullptr);
    connection->SetType(WOWC_TYPE_STREAM);
    connection->Connect("224.0.0.1", 3724, 5000);

    Check(response.m_cantConnect == 1, "WCCantConnect");
    Check(response.m_connected == 0 && response.m_disconnected == 0, "no WCConnected/WCDisconnected");
    Check(connection->m_connState == WOWC_DISCONNECTED && connection->m_sock == -1, "state is WOWC_DISCONNECTED, socket closed");

    connection->SetResponse(nullptr, false);
    connection->Release();
}

int main(int argc, char* argv[]) {
    StormRtlInitialize();
    StormInitialize();
    OsTimeManagerCreate(OsTimeManager::BestAvailable);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // A write to a dead socket must fail with EPIPE (SO_NOSIGPIPE), not kill the process: leave SIGPIPE at default.

    TestNetAddr();
    TestVersionChecksum();

    WowConnection::InitOsNet(nullptr, nullptr, 1, false);

    TestStreamEcho();
    TestHostNameAndClientDisconnect();
    TestMessages();
    TestRefused();
    TestImmediateFailure();

    WowConnection::DestroyOsNet();
    OsTimeManagerDestroy();

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
