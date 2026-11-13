#include "DownloadManager.h"
#include "ThreadPool.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void updateMaximum(std::atomic<std::size_t>& maximum, std::size_t value) {
    std::size_t current = maximum.load();
    while (value > current && !maximum.compare_exchange_weak(current, value)) {
    }
}

class ScopedTestDirectory {
public:
    ScopedTestDirectory() : original_(std::filesystem::current_path()) {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     ("multithread-self-test-" + std::to_string(suffix));
        std::filesystem::create_directories(directory_);
        std::filesystem::current_path(directory_);
    }

    ~ScopedTestDirectory() {
        std::error_code error;
        std::filesystem::current_path(original_, error);
        const std::filesystem::path tempRoot =
            std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), error);
        const std::filesystem::path target =
            std::filesystem::weakly_canonical(directory_, error);
        if (!error && target.parent_path() == tempRoot &&
            target.filename().string().rfind("multithread-self-test-", 0) == 0) {
            std::filesystem::remove_all(target, error);
        }
    }

private:
    std::filesystem::path original_;
    std::filesystem::path directory_;
};

class HttpTestServer {
public:
    HttpTestServer() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed.");
        }
        winsockStarted_ = true;

        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            throw std::runtime_error("Could not create test server socket.");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
            throw std::runtime_error("Could not bind/listen on the local test port.");
        }

        int addressLength = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                        &addressLength) == SOCKET_ERROR) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
            throw std::runtime_error("Could not read the local test port.");
        }
        port_ = ntohs(address.sin_port);
        acceptThread_ = std::thread(&HttpTestServer::acceptClients, this);
    }

    ~HttpTestServer() {
        stopping_.store(true);
        SOCKET wakeSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (wakeSocket != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            connect(wakeSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            closesocket(wakeSocket);
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
        }
        for (std::thread& client : clientThreads_) {
            if (client.joinable()) {
                client.join();
            }
        }
        if (winsockStarted_) {
            WSACleanup();
        }
    }

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    void acceptClients() {
        while (!stopping_.load()) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            if (stopping_.load()) {
                closesocket(client);
                return;
            }
            clientThreads_.emplace_back(&HttpTestServer::serveClient, this, client);
        }
    }

    void serveClient(SOCKET client) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const int received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                closesocket(client);
                return;
            }
            request.append(buffer, static_cast<std::size_t>(received));
        }

        const std::size_t firstSpace = request.find(' ');
        const std::size_t secondSpace = request.find(' ', firstSpace + 1);
        const std::string path = request.substr(firstSpace + 1,
                                                secondSpace - firstSpace - 1);
        if (path.find("/slow") == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        std::string response;
        if (path.find("/redirect") == 0) {
            response = "HTTP/1.1 302 Found\r\nLocation: " + baseUrl() +
                       "/ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else if (path.find("/not-found") == 0) {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                       "Connection: close\r\n\r\n";
        } else {
            const std::string body = path.find("/slow") == 0
                ? std::string(1024, 'x')
                : "hello world";
            response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
        }

        std::size_t sentTotal = 0;
        while (sentTotal < response.size()) {
            const int sent = send(client, response.data() + sentTotal,
                                  static_cast<int>(response.size() - sentTotal), 0);
            if (sent <= 0) {
                break;
            }
            sentTotal += static_cast<std::size_t>(sent);
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    bool winsockStarted_ = false;
    std::atomic<bool> stopping_{false};
    std::thread acceptThread_;
    std::vector<std::thread> clientThreads_;
};

void testThreadPool() {
    constexpr std::size_t workers = 4;
    constexpr std::size_t taskCount = 64;
    std::array<std::atomic<std::size_t>, taskCount> taskRuns{};
    for (auto& runs : taskRuns) {
        runs.store(0);
    }
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> peak{0};

    {
        ThreadPool pool(workers);
        for (std::size_t i = 0; i < taskCount; ++i) {
            pool.enqueue([&, i] {
                const std::size_t current = active.fetch_add(1) + 1;
                updateMaximum(peak, current);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                active.fetch_sub(1);
                taskRuns[i].fetch_add(1);
                completed.fetch_add(1);
            });
        }
        pool.waitIdle();
    }

    require(completed.load() == taskCount, "Thread pool lost or repeated a task.");
    for (const auto& runs : taskRuns) {
        require(runs.load() == 1, "A thread-pool task did not run exactly once.");
    }
    require(peak.load() > 0 && peak.load() <= workers,
            "Thread pool exceeded its configured worker count.");

    std::atomic<std::size_t> drained{0};
    {
        ThreadPool pool(3);
        for (std::size_t i = 0; i < 32; ++i) {
            pool.enqueue([&] { drained.fetch_add(1); });
        }
    }
    require(drained.load() == 32, "Thread pool shutdown did not drain queued tasks.");

    bool rejectedZeroWorkers = false;
    try {
        ThreadPool invalidPool(0);
    } catch (const std::invalid_argument&) {
        rejectedZeroWorkers = true;
    }
    require(rejectedZeroWorkers, "Thread pool accepted a zero-worker configuration.");
}

void testDownloadsAndHistory(HttpTestServer& server) {
    const std::string base = server.baseUrl();
    std::string error;
    {
        DownloadManager manager(4);
        require(!manager.addUrl("ftp://example.com/file", error),
                "Download manager accepted a non-HTTP URL.");
        require(manager.addUrl(base + "/ok", error), "Could not queue local success test.");
        require(manager.addUrl(base + "/redirect", error),
                "Could not queue local redirect test.");
        require(manager.addUrl(base + "/not-found", error),
                "Could not queue local 404 test.");
        require(manager.addUrl("http://127.0.0.1:1/unreachable", error),
                "Could not queue local connection-failure test.");
    } // Manager shutdown waits for all four downloads.

    std::vector<DownloadHistoryEntry> history;
    {
        DownloadManager restartedManager(2);
        history = restartedManager.getHistory(error);
    }
    require(error.empty(), "Could not read SQLite history after restart: " + error);
    require(history.size() == 4, "History did not persist exactly four valid URL tasks.");

    const auto findByPath = [&history](const std::string& suffix)
        -> const DownloadHistoryEntry& {
        const auto it = std::find_if(history.begin(), history.end(),
            [&suffix](const DownloadHistoryEntry& item) {
                return item.url.size() >= suffix.size() &&
                       item.url.compare(item.url.size() - suffix.size(),
                                        suffix.size(), suffix) == 0;
            });
        if (it == history.end()) {
            throw std::runtime_error("Could not find history record for " + suffix);
        }
        return *it;
    };

    const DownloadHistoryEntry& success = findByPath("/ok");
    const DownloadHistoryEntry& redirect = findByPath("/redirect");
    const DownloadHistoryEntry& notFound = findByPath("/not-found");
    const DownloadHistoryEntry& unreachable = findByPath("/unreachable");
    require(success.status == DownloadStatus::Completed,
            "Successful response was not marked Completed.");
    require(redirect.status == DownloadStatus::Completed,
            "Redirect response was not followed successfully.");
    require(notFound.status == DownloadStatus::Failed && !notFound.errorMessage.empty(),
            "HTTP 404 was not recorded as a failure with an error.");
    require(unreachable.status == DownloadStatus::Failed &&
                !unreachable.errorMessage.empty(),
            "Connection failure was not recorded in history.");
    for (const DownloadHistoryEntry& entry : history) {
        require(!entry.createdAt.empty(), "History record is missing its timestamp.");
    }
    require(success.progressPercent == 100.0 && redirect.progressPercent == 100.0,
            "Completed transfer progress was not persisted as 100 percent.");

    require(std::filesystem::file_size(
                std::filesystem::path("downloads") /
                    (std::to_string(success.id) + "_" + success.filename)) == 11,
            "Successful response did not produce the expected file.");
    require(std::filesystem::file_size(
                std::filesystem::path("downloads") /
                    (std::to_string(redirect.id) + "_" + redirect.filename)) == 11,
            "Redirected response did not produce the expected file.");
    require(!std::filesystem::exists(
                std::filesystem::path("downloads") /
                    (std::to_string(notFound.id) + "_" + notFound.filename)),
            "Failed HTTP response left a partial output file.");
}

void testBenchmark(HttpTestServer& server) {
    std::string error;
    DownloadManager manager(3);
    std::vector<std::string> urls;
    for (int i = 0; i < 8; ++i) {
        urls.push_back(server.baseUrl() + "/slow/" + std::to_string(i));
    }

    const std::vector<BenchmarkResult> results = manager.benchmark(urls, 1, error);
    require(error.empty() && results.size() == 5,
            "Eight-URL benchmark did not produce all configurations.");
    for (const BenchmarkResult& result : results) {
        require(result.successfulDownloads == 8 && result.failedDownloads == 0,
                "Benchmark did not complete all eight local transfers.");
        require(result.validRounds == 1,
                "Benchmark did not accept a matching successful round.");
        const std::size_t workerLimit = result.sequential ? 1 : result.workerCount;
        require(result.peakSimultaneousDownloads <= workerLimit,
                "Benchmark exceeded its configured worker limit.");
    }
    require(results.back().peakSimultaneousDownloads == 8,
            "Eight-worker benchmark did not reach eight simultaneous transfers.");

    const std::vector<BenchmarkResult> failedResults =
        manager.benchmark({server.baseUrl() + "/not-found"}, 1, error);
    require(error.empty() && failedResults.size() == 5,
            "Failure-case benchmark did not produce all configurations.");
    for (const BenchmarkResult& result : failedResults) {
        require(result.failedDownloads == 1 && result.validRounds == 0,
                "Failed benchmark round was incorrectly considered comparable.");
        require(result.medianSpeedup == 0.0 &&
                    result.medianTimeReductionPercent == 0.0,
                "Failed benchmark data produced a speedup result.");
    }

    const std::vector<DownloadHistoryEntry> history = manager.getHistory(error);
    require(error.empty() && history.size() == 4,
            "Benchmark transfers were incorrectly added to download history.");
}
} // namespace

int main() {
    try {
        ScopedTestDirectory isolatedDirectory;
        testThreadPool();
        HttpTestServer server;
        testDownloadsAndHistory(server);
        testBenchmark(server);
        std::cout << "All MultiThread self-tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Self-test failed: " << exception.what() << '\n';
        return 1;
    }
}
