// Stage E7 test for the macOS HTTP layer: Blizzard::URL::DownloadURL (streaming) and OsURLDownload (glue).
//
//   HttpTest [--no-internet]
//
// Runs a small HTTP/1.1 server on 127.0.0.1 in a thread of this process (GET/HEAD/POST, Range, If-Modified-Since,
// 404/403, a slow endpoint) and checks the callback sequence and statuses of both APIs against it: full GET,
// range request (206 with the exact bytes), HEAD (WantData false), POST body and content type, cookie header,
// 304, 404, 403, timeout, aborting from ReceivedData, Stop(), refused connection and bad URLs. Unless
// --no-internet is given, it also downloads https://example.com/ through both APIs and resolves a host that does
// not exist. Exit code 0 when everything passed.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"
#include "Engine/Source/Os/OsURLDownload.h"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_failures;

static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);

    if (!ok) {
        s_failures++;
    }
}

// --- Test server ---------------------------------------------------------------------------------------------

static std::string s_body;
static int s_serverSocket = -1;
static uint16_t s_port;
static std::string s_lastRequest;
static std::mutex s_lastRequestMutex;

static std::string HeaderValue(const std::string& request, const char* name) {
    std::string lower = request;
    std::string key = std::string("\r\n") + name + ":";

    for (char& c : lower) {
        c = static_cast<char>(tolower(c));
    }

    for (char& c : key) {
        c = static_cast<char>(tolower(c));
    }

    size_t pos = lower.find(key);

    if (pos == std::string::npos) {
        return "";
    }

    pos += key.size();
    size_t end = request.find("\r\n", pos);
    std::string value = request.substr(pos, end - pos);

    while (!value.empty() && value[0] == ' ') {
        value.erase(0, 1);
    }

    return value;
}

static void SendAll(int fd, const std::string& data) {
    size_t sent = 0;

    while (sent < data.size()) {
        ssize_t result = send(fd, data.data() + sent, data.size() - sent, 0);

        if (result <= 0) {
            return;
        }

        sent += result;
    }
}

static void HandleClient(int fd) {
    std::string request;
    char buffer[4096];

    while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t got = recv(fd, buffer, sizeof(buffer), 0);

        if (got <= 0) {
            close(fd);
            return;
        }

        request.append(buffer, got);
    }

    size_t headerEnd = request.find("\r\n\r\n") + 4;
    size_t contentLength = strtoul(HeaderValue(request, "Content-Length").c_str(), nullptr, 10);

    while (request.size() < headerEnd + contentLength) {
        ssize_t got = recv(fd, buffer, sizeof(buffer), 0);

        if (got <= 0) {
            break;
        }

        request.append(buffer, got);
    }

    {
        std::lock_guard<std::mutex> lock(s_lastRequestMutex);
        s_lastRequest = request;
    }

    std::string method = request.substr(0, request.find(' '));
    size_t pathStart = request.find(' ') + 1;
    std::string path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
    const char* lastModified = "Wed, 21 Oct 2015 07:28:00 GMT";
    std::string response;

    if (path == "/data.bin") {
        std::string ifModifiedSince = HeaderValue(request, "If-Modified-Since");
        std::string range = HeaderValue(request, "Range");

        if (ifModifiedSince == lastModified) {
            response = "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n";
        } else if (!range.empty()) {
            long long start = 0;
            long long end = -1;
            sscanf(range.c_str(), "bytes=%lld-%lld", &start, &end);

            if (end < 0 || end >= static_cast<long long>(s_body.size())) {
                end = s_body.size() - 1;
            }

            std::string part = s_body.substr(start, end - start + 1);
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%zu\r\nContent-Length: %zu\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n", start, end, s_body.size(), part.size(), lastModified);
            response = header;

            if (method != "HEAD") {
                response += part;
            }
        } else {
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n", s_body.size(), lastModified);
            response = header;

            if (method != "HEAD") {
                response += s_body;
            }
        }
    } else if (path == "/echo") {
        std::string body = request.substr(headerEnd);
        std::string echoed = method + "|" + HeaderValue(request, "Content-Type") + "|" + HeaderValue(request, "Cookie") + "|" + body;
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", echoed.size());
        response = std::string(header) + echoed;
    } else if (path == "/forbidden") {
        response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (path == "/slow") {
        // Headers, then nothing for 3 s.
        SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        close(fd);
        return;
    } else {
        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found";
    }

    SendAll(fd, response);
    shutdown(fd, SHUT_WR);
    close(fd);
}

static bool StartServer() {
    s_body.resize(100000);

    for (size_t i = 0; i < s_body.size(); i++) {
        s_body[i] = static_cast<char>((i * 7 + i / 251) & 0xFF);
    }

    s_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(s_serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(s_serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(s_serverSocket, 16)) {
        return false;
    }

    socklen_t length = sizeof(address);
    getsockname(s_serverSocket, reinterpret_cast<sockaddr*>(&address), &length);
    s_port = ntohs(address.sin_port);

    std::thread([] {
        for (;;) {
            int client = accept(s_serverSocket, nullptr, nullptr);

            if (client < 0) {
                return;
            }

            std::thread(HandleClient, client).detach();
        }
    }).detach();

    return true;
}

static std::string LocalUrl(const char* path) {
    return "http://127.0.0.1:" + std::to_string(s_port) + path;
}

// --- Blizzard::URL ---------------------------------------------------------------------------------------------

class TestCallback : public Blizzard::URL::DownloadCallback {
    public:
    // Configuration.
    bool m_wantData = true;
    int32_t m_timeout = 10000;
    ByteRange m_range = { -1, -1 };
    const char* m_ifModifiedSince = nullptr;
    const char* m_postData = nullptr;
    const char* m_postContentType = nullptr;
    Cookie m_cookie = { nullptr, nullptr };
    int64_t m_abortAfter = -1;
    int64_t m_stopAfter = -1;

    // Results.
    std::string m_data;
    std::string m_lastModified;
    int64_t m_contentLength = -1;
    int32_t m_httpStatus = -1;
    bool m_connected = false;
    int32_t m_completedCount = 0;
    Status m_status = DOWNLOAD_UNKNOWN_ERROR;
    std::vector<std::string> m_order;
    int32_t m_maxChunk = 0;

    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_completed = false;

    const char* WantIfModifiedSince() override { return m_ifModifiedSince; }
    bool WantData() override { return m_wantData; }
    void GetLastModified(const char* lastModified) override { m_lastModified = lastModified; m_order.push_back("lastmodified"); }
    void GetContentLength(int64_t length) override { m_contentLength = length; m_order.push_back("length"); }
    void ConnectedOK() override { m_connected = true; m_order.push_back("connected"); }
    void GetHTTPStatusCode(int32_t code) override { m_httpStatus = code; m_order.push_back("status"); }
    int32_t GetTimeout() override { return m_timeout; }
    ByteRange GetByteRange() override { return m_range; }
    bool Stop() override { return m_stopAfter >= 0 && static_cast<int64_t>(m_data.size()) >= m_stopAfter; }
    const char* GetPostData() override { return m_postData; }
    const char* GetPostContentType() override { return m_postContentType; }
    Cookie GetCookie() override { return m_cookie; }

    bool ReceivedData(const void* data, int32_t size) override {
        if (m_order.empty() || m_order.back() != "data") {
            m_order.push_back("data");
        }

        m_maxChunk = size > m_maxChunk ? size : m_maxChunk;
        m_data.append(static_cast<const char*>(data), size);
        return m_abortAfter < 0 || static_cast<int64_t>(m_data.size()) < m_abortAfter;
    }

    void Completed(Status status) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = status;
        m_completedCount++;
        m_order.push_back("completed");
        m_completed = true;
        m_condition.notify_all();
    }

    bool Wait(int32_t seconds = 20) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_completed; });
    }

    std::string Order() {
        std::string order;

        for (const std::string& step : m_order) {
            order += (order.empty() ? "" : ",") + step;
        }

        return order;
    }
};

static bool Download(const std::string& url, TestCallback& callback, const char* title) {
    std::printf("\n== %s (%s)\n", title, url.c_str());
    bool started = Blizzard::URL::DownloadURL(url.c_str(), &callback);
    Check(started, "DownloadURL started");

    if (!started) {
        return false;
    }

    bool completed = callback.Wait();
    Check(completed, "Completed called");
    // A second Completed would come right after the first.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("     http %d, status %d, %zu bytes, order %s\n", callback.m_httpStatus, callback.m_status, callback.m_data.size(), callback.Order().c_str());
    Check(callback.m_completedCount == 1, "Completed called exactly once");
    return completed;
}

static std::string LastRequest() {
    std::lock_guard<std::mutex> lock(s_lastRequestMutex);
    return s_lastRequest;
}

static void TestBlizzardURL(bool internet) {
    {
        TestCallback callback;
        Download(LocalUrl("/data.bin"), callback, "GET");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS && callback.m_httpStatus == 200, "200, DOWNLOAD_SUCCESS");
        Check(callback.m_data == s_body, "all 100000 bytes, unchanged");
        Check(callback.m_maxChunk <= 16384, "data in blocks of at most 16 KB");
        Check(callback.m_contentLength == 100000 && callback.m_lastModified == "Wed, 21 Oct 2015 07:28:00 GMT", "Content-Length and Last-Modified");
        Check(callback.Order() == "connected,status,lastmodified,length,data,completed", "callback order");
        Check(LastRequest().find("GET /data.bin ") == 0 && LastRequest().find("Blizzard Web Client") != std::string::npos, "GET with the Blizzard Web Client user agent");
    }

    {
        TestCallback callback;
        callback.m_range = { 1000, 1999 };
        Download(LocalUrl("/data.bin"), callback, "Range 1000-1999");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS && callback.m_httpStatus == 206, "206, DOWNLOAD_SUCCESS");
        Check(callback.m_data == s_body.substr(1000, 1000), "the 1000 requested bytes");
        std::printf("     Range header: \"%s\"\n", HeaderValue(LastRequest(), "Range").c_str());
        Check(HeaderValue(LastRequest(), "Range") == "bytes=1000-1999", "Range: bytes=1000-1999");
    }

    {
        TestCallback callback;
        callback.m_range = { 99990, -1 };
        Download(LocalUrl("/data.bin"), callback, "Range 99990-");
        Check(callback.m_httpStatus == 206 && callback.m_data == s_body.substr(99990), "open-ended range: the last 10 bytes");
    }

    {
        TestCallback callback;
        callback.m_wantData = false;
        Download(LocalUrl("/data.bin"), callback, "HEAD (WantData false)");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS && callback.m_data.empty() && callback.m_contentLength == 100000, "DOWNLOAD_SUCCESS, length, no data");
        Check(LastRequest().find("HEAD ") == 0, "HEAD request");
    }

    {
        TestCallback callback;
        callback.m_postData = "name=value&x=1";
        callback.m_postContentType = "application/x-www-form-urlencoded";
        callback.m_cookie = { "session", "abc123" };
        Download(LocalUrl("/echo"), callback, "POST with cookie");
        Check(callback.m_data == "POST|application/x-www-form-urlencoded|session=abc123|name=value&x=1", "method, content type, cookie and body reach the server");
    }

    {
        TestCallback callback;
        callback.m_ifModifiedSince = "Wed, 21 Oct 2015 07:28:00 GMT";
        Download(LocalUrl("/data.bin"), callback, "If-Modified-Since");
        Check(callback.m_httpStatus == 304 && callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_NOT_MODIFIED && callback.m_data.empty(), "304 → DOWNLOAD_NOT_MODIFIED");
    }

    {
        TestCallback callback;
        Download(LocalUrl("/missing"), callback, "404");
        Check(callback.m_httpStatus == 404 && callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_NOT_FOUND && callback.m_data.empty(), "404 → DOWNLOAD_NOT_FOUND, body not passed on");
    }

    {
        TestCallback callback;
        Download(LocalUrl("/forbidden"), callback, "403");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_FORBIDDEN, "403 → DOWNLOAD_FORBIDDEN");
    }

    {
        TestCallback callback;
        callback.m_abortAfter = 20000;
        Download(LocalUrl("/data.bin"), callback, "ReceivedData returns false");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_ABORTED && callback.m_data.size() < s_body.size(), "DOWNLOAD_ABORTED, transfer stopped");
    }

    {
        TestCallback callback;
        callback.m_stopAfter = 1;
        Download(LocalUrl("/data.bin"), callback, "Stop() returns true");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_STOPPED && callback.m_data.size() <= 16384, "DOWNLOAD_STOPPED after the first block");
    }

    {
        TestCallback callback;
        callback.m_timeout = 1000;
        Download(LocalUrl("/slow"), callback, "timeout 1 s");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_TIMEOUT, "DOWNLOAD_TIMEOUT");
    }

    {
        // A port nothing listens on.
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        close(probe);

        TestCallback callback;
        Download("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/", callback, "connection refused");
        Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_CANNOT_CONNECT && !callback.m_connected, "DOWNLOAD_CANNOT_CONNECT, ConnectedOK not called");
    }

    {
        std::printf("\n== bad URLs\n");
        TestCallback callback;
        Check(!Blizzard::URL::DownloadURL("not a url", &callback), "\"not a url\" → false");
        Check(!Blizzard::URL::DownloadURL("ftp://example.com/x", &callback), "ftp:// → false");
        Check(callback.m_completedCount == 0, "no Completed for a request that did not start");
    }

    if (internet) {
        {
            TestCallback callback;
            Download("https://example.com/", callback, "HTTPS, public server");
            Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS && callback.m_httpStatus == 200, "200, DOWNLOAD_SUCCESS");
            Check(callback.m_data.find("Example Domain") != std::string::npos, "page contains \"Example Domain\"");
        }

        {
            TestCallback callback;
            Download("http://wowdecomp-no-such-host.invalid/", callback, "unknown host");
            Check(callback.m_status == Blizzard::URL::DownloadCallback::DOWNLOAD_NAME_NOT_RESOLVED, "DOWNLOAD_NAME_NOT_RESOLVED");
        }
    }
}

// --- OsURLDownload --------------------------------------------------------------------------------------------

struct OsDownloadResult {
    std::string m_data;
    int32_t m_dataCalls = 0;
    int32_t m_emptyDataCalls = 0;
    int32_t m_doneCalls = 0;
    int32_t m_stopAfter = -1;
    HTTPError::ErrorCode m_error = HTTPError::NoError;
    std::mutex m_mutex;
    std::condition_variable m_condition;
};

static bool OsCallback(void* param, const char* data, uint32_t size, bool done, HTTPError::ErrorCode error) {
    OsDownloadResult* result = static_cast<OsDownloadResult*>(param);

    if (done) {
        std::lock_guard<std::mutex> lock(result->m_mutex);
        result->m_doneCalls++;
        result->m_error = error;
        result->m_condition.notify_all();
        return true;
    }

    result->m_dataCalls++;
    result->m_emptyDataCalls += size == 0;
    result->m_data.append(data, size);
    return result->m_stopAfter < 0 || static_cast<int32_t>(result->m_data.size()) < result->m_stopAfter;
}

static bool OsDownload(const std::string& url, OsDownloadResult& result, const char* title, int32_t timeout = 0) {
    std::printf("\n== OsURLDownload: %s (%s)\n", title, url.c_str());
    bool started = timeout ? OsUrlDownload::DownloadString(url.c_str(), OsCallback, &result, timeout) : OsURLDownload(url.c_str(), OsCallback, &result);
    Check(started, "started");

    if (!started) {
        return false;
    }

    std::unique_lock<std::mutex> lock(result.m_mutex);
    bool done = result.m_condition.wait_for(lock, std::chrono::seconds(20), [&] { return result.m_doneCalls > 0; });
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("     error %d, %zu bytes in %d data calls\n", result.m_error, result.m_data.size(), result.m_dataCalls);
    Check(done && result.m_doneCalls == 1, "done callback exactly once");
    return done;
}

static void TestOsURLDownload(bool internet) {
    {
        OsDownloadResult result;
        OsDownload(LocalUrl("/data.bin"), result, "GET");
        Check(result.m_error == HTTPError::NoError && result.m_data == s_body, "NoError, all bytes");
        Check(result.m_emptyDataCalls == 1, "one final empty data call, as the last WinINet read");
    }

    {
        OsDownloadResult result;
        OsDownload(LocalUrl("/missing"), result, "404");
        Check(result.m_error == HTTPError::NotFound && result.m_dataCalls == 0, "NotFound, no data");
    }

    {
        OsDownloadResult result;
        OsDownload(LocalUrl("/forbidden"), result, "403");
        Check(result.m_error == HTTPError::Failed, "Failed");
    }

    {
        OsDownloadResult result;
        result.m_stopAfter = 1;
        OsDownload(LocalUrl("/data.bin"), result, "callback returns false");
        Check(result.m_error == HTTPError::NoError && result.m_dataCalls == 1, "stopped after the first block, done with NoError");
    }

    {
        OsDownloadResult result;
        OsDownload(LocalUrl("/slow"), result, "timeout 1 s", 1000);
        Check(result.m_error == HTTPError::Timeout, "Timeout");
    }

    {
        std::printf("\n== OsURLDownload: bad URL\n");
        OsDownloadResult result;
        Check(!OsURLDownload("nonsense", OsCallback, &result), "\"nonsense\" → false");
    }

    if (internet) {
        OsDownloadResult result;
        OsDownload("https://example.com/", result, "HTTPS, public server");
        Check(result.m_error == HTTPError::NoError && result.m_data.find("Example Domain") != std::string::npos, "NoError, page contains \"Example Domain\"");
    }
}

int main(int argc, char** argv) {
    bool internet = !(argc > 1 && !strcmp(argv[1], "--no-internet"));

    StormRtlInitialize();
    StormInitialize();

    if (!StartServer()) {
        std::printf("FAIL cannot start the test server\n");
        return 1;
    }

    std::printf("test server on port %u\n", s_port);

    TestBlizzardURL(internet);
    TestOsURLDownload(internet);

    std::printf("\n%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
