#include "DownloadManager.h"
#include "ThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <curl/curl.h>
#include <sqlite3.h>

namespace {
struct TransferContext {
    std::function<void(double, double)> progress;
    std::ofstream output;
};

struct BenchmarkCounters {
    std::atomic<std::uint64_t> bytesDownloaded{0};
    std::atomic<std::size_t> successfulDownloads{0};
    std::atomic<std::size_t> failedDownloads{0};
    std::atomic<std::size_t> activeDownloads{0};
    std::atomic<std::size_t> peakSimultaneousDownloads{0};
};

std::size_t discardBenchmarkData(char*, std::size_t size, std::size_t count,
                                 void* userData) {
    const std::size_t bytes = size * count;
    auto* counters = static_cast<BenchmarkCounters*>(userData);
    counters->bytesDownloaded.fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

void benchmarkDownload(const std::string& url, BenchmarkCounters& counters) {
    const std::size_t active =
        counters.activeDownloads.fetch_add(1, std::memory_order_relaxed) + 1;
    std::size_t peak = counters.peakSimultaneousDownloads.load(std::memory_order_relaxed);
    while (active > peak &&
           !counters.peakSimultaneousDownloads.compare_exchange_weak(
               peak, active, std::memory_order_relaxed)) {
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        counters.failedDownloads.fetch_add(1, std::memory_order_relaxed);
        counters.activeDownloads.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MultiThread/0.1 benchmark");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardBenchmarkData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &counters);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (result == CURLE_OK) {
        counters.successfulDownloads.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters.failedDownloads.fetch_add(1, std::memory_order_relaxed);
    }
    counters.activeDownloads.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t writeData(char* data, std::size_t size, std::size_t count, void* userData) {
    const std::size_t bytes = size * count;
    auto* context = static_cast<TransferContext*>(userData);
    context->output.write(data, static_cast<std::streamsize>(bytes));
    return context->output ? bytes : 0;
}

int reportProgress(void* userData, curl_off_t total, curl_off_t downloaded,
                   curl_off_t, curl_off_t) {
    auto* context = static_cast<TransferContext*>(userData);
    context->progress(static_cast<double>(downloaded),
                      static_cast<double>(total));
    return 0;
}

DownloadStatus statusFromString(const unsigned char* value) {
    if (value == nullptr) {
        return DownloadStatus::Failed;
    }
    const std::string status(reinterpret_cast<const char*>(value));
    if (status == "Running") return DownloadStatus::Running;
    if (status == "Completed") return DownloadStatus::Completed;
    if (status == "Failed") return DownloadStatus::Failed;
    return DownloadStatus::Pending;
}

double medianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[middle - 1] + values[middle]) / 2.0;
    }
    return values[middle];
}
} // namespace

DownloadManager::DownloadManager(std::size_t workerCount) {
    std::error_code filesystemError;
    std::filesystem::create_directories("database", filesystemError);
    if (filesystemError) {
        throw std::runtime_error("Could not create the database directory: " +
                                 filesystemError.message());
    }

    if (sqlite3_open("database/downloads.db", &database_) != SQLITE_OK) {
        const std::string message = database_ != nullptr
            ? sqlite3_errmsg(database_)
            : "Could not open the SQLite database.";
        if (database_ != nullptr) {
            sqlite3_close(database_);
            database_ = nullptr;
        }
        throw std::runtime_error("Could not open the download history database: " + message);
    }
    sqlite3_busy_timeout(database_, 5000);

    const char* createTableSql =
        "CREATE TABLE IF NOT EXISTS downloads ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "url TEXT NOT NULL,"
        "filename TEXT NOT NULL,"
        "status TEXT NOT NULL,"
        "progress REAL NOT NULL DEFAULT 0,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "error_message TEXT NOT NULL DEFAULT ''"
        ");";
    char* databaseError = nullptr;
    if (sqlite3_exec(database_, createTableSql, nullptr, nullptr, &databaseError) != SQLITE_OK) {
        const std::string message = databaseError != nullptr
            ? databaseError
            : sqlite3_errmsg(database_);
        sqlite3_free(databaseError);
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("Could not create the download history table: " + message);
    }

    const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) {
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("libcurl initialization failed.");
    }
    try {
        pool_ = std::make_unique<ThreadPool>(workerCount);
    } catch (...) {
        curl_global_cleanup();
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

DownloadManager::~DownloadManager() {
    // Destroying the pool drains queued tasks and joins workers before curl cleanup.
    pool_.reset();
    if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
    curl_global_cleanup();
}

bool DownloadManager::isHttpUrl(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return false;
    }

    std::string scheme = url.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (scheme != "http" && scheme != "https") {
        return false;
    }

    const std::size_t hostStart = schemeEnd + 3;
    const std::size_t hostEnd = url.find_first_of("/?#", hostStart);
    const std::string host = url.substr(hostStart, hostEnd - hostStart);
    if (host.empty()) {
        return false;
    }

    return url.find_first_of(" \t\r\n") == std::string::npos;
}

std::string DownloadManager::filenameFromUrl(const std::string& url,
                                             std::size_t taskNumber) {
    const std::size_t authorityStart = url.find("://") + 3;
    const std::size_t pathStart = url.find('/', authorityStart);
    if (pathStart == std::string::npos) {
        return "download_" + std::to_string(taskNumber);
    }

    const std::size_t pathEnd = url.find_first_of("?#", pathStart);
    const std::string path = url.substr(pathStart, pathEnd - pathStart);
    const std::size_t nameStart = path.find_last_of('/');
    std::string filename = path.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    if (filename.empty() || filename == "." || filename == "..") {
        return "download_" + std::to_string(taskNumber);
    }

    // Keep URL path data from being interpreted as a local Windows path.
    for (char& ch : filename) {
        if (std::string("<>:\"/\\|?*").find(ch) != std::string::npos) {
            ch = '_';
        }
    }
    while (!filename.empty() && (filename.back() == '.' || filename.back() == ' ')) {
        filename.pop_back();
    }
    if (filename.empty()) {
        return "download_" + std::to_string(taskNumber);
    }
    return filename;
}

bool DownloadManager::addUrl(const std::string& url, std::string& errorMessage) {
    if (!isHttpUrl(url)) {
        errorMessage = "Enter a valid HTTP or HTTPS URL with a host name.";
        return false;
    }

    std::size_t id;
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = tasks_.size() + 1;
        filename = filenameFromUrl(url, id);

        sqlite3_stmt* statement = nullptr;
        const char* insertSql =
            "INSERT INTO downloads (url, filename, status, progress, error_message) "
            "VALUES (?, ?, 'Pending', 0, '');";
        if (sqlite3_prepare_v2(database_, insertSql, -1, &statement, nullptr) != SQLITE_OK) {
            errorMessage = sqlite3_errmsg(database_);
            return false;
        }
        sqlite3_bind_text(statement, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, filename.c_str(), -1, SQLITE_TRANSIENT);
        const int insertResult = sqlite3_step(statement);
        if (insertResult != SQLITE_DONE) {
            errorMessage = sqlite3_errmsg(database_);
            sqlite3_finalize(statement);
            return false;
        }
        sqlite3_finalize(statement);

        DownloadTask task{};
        task.id = id;
        task.databaseId = sqlite3_last_insert_rowid(database_);
        task.url = url;
        task.filename = filename;
        tasks_.push_back(std::move(task));
    }

    try {
        pool_->enqueue([this, id, url, filename] {
            runDownload(id, url, filename);
        });
    } catch (const std::exception& exception) {
        markFailed(id, exception.what());
        errorMessage = exception.what();
        return false;
    }
    errorMessage.clear();
    return true;
}

std::vector<DownloadTask> DownloadManager::getTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_;
}

std::vector<DownloadHistoryEntry> DownloadManager::getHistory(
    std::string& errorMessage) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadHistoryEntry> history;
    sqlite3_stmt* statement = nullptr;
    const char* selectSql =
        "SELECT id, url, filename, status, progress, created_at, error_message "
        "FROM downloads ORDER BY id DESC;";
    if (sqlite3_prepare_v2(database_, selectSql, -1, &statement, nullptr) != SQLITE_OK) {
        errorMessage = sqlite3_errmsg(database_);
        return history;
    }

    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        DownloadHistoryEntry entry{};
        entry.id = sqlite3_column_int64(statement, 0);
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        entry.filename = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        entry.status = statusFromString(sqlite3_column_text(statement, 3));
        entry.progressPercent = sqlite3_column_double(statement, 4);
        entry.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5));
        const unsigned char* error = sqlite3_column_text(statement, 6);
        if (error != nullptr) {
            entry.errorMessage = reinterpret_cast<const char*>(error);
        }
        history.push_back(std::move(entry));
    }

    if (result != SQLITE_DONE) {
        errorMessage = sqlite3_errmsg(database_);
        history.clear();
    } else {
        errorMessage.clear();
    }
    sqlite3_finalize(statement);
    return history;
}

std::vector<BenchmarkResult> DownloadManager::benchmark(
    const std::vector<std::string>& urls, std::size_t roundCount,
    std::string& errorMessage) {
    if (urls.empty()) {
        errorMessage = "Add at least one URL to benchmark.";
        return {};
    }
    if (roundCount == 0) {
        errorMessage = "Benchmark must run for at least one round.";
        return {};
    }
    for (const std::string& url : urls) {
        if (!isHttpUrl(url)) {
            errorMessage = "Every benchmark URL must be a valid HTTP or HTTPS URL.";
            return {};
        }
    }

    // Finish regular downloads first so they do not skew the benchmark.
    pool_->waitIdle();

    constexpr std::array<std::size_t, 5> workerCounts{{1, 1, 2, 4, 8}};
    std::random_device randomDevice;
    std::mt19937 randomGenerator(randomDevice());
    std::vector<std::array<BenchmarkResult, 5>> rounds;
    rounds.reserve(roundCount);

    const auto measure = [&urls](bool sequential, std::size_t workerCount) {
        BenchmarkCounters counters;
        const auto start = std::chrono::steady_clock::now();

        if (sequential) {
            for (const std::string& url : urls) {
                benchmarkDownload(url, counters);
            }
        } else {
            ThreadPool benchmarkPool(workerCount);
            for (const std::string& url : urls) {
                benchmarkPool.enqueue([&counters, url] {
                    benchmarkDownload(url, counters);
                });
            }
            benchmarkPool.waitIdle();
        }

        const auto end = std::chrono::steady_clock::now();
        BenchmarkResult result;
        result.sequential = sequential;
        result.workerCount = workerCount;
        result.elapsedSeconds = std::chrono::duration<double>(end - start).count();
        result.bytesDownloaded = counters.bytesDownloaded.load(std::memory_order_relaxed);
        result.successfulDownloads =
            counters.successfulDownloads.load(std::memory_order_relaxed);
        result.failedDownloads = counters.failedDownloads.load(std::memory_order_relaxed);
        result.peakSimultaneousDownloads =
            counters.peakSimultaneousDownloads.load(std::memory_order_relaxed);
        return result;
    };

    for (std::size_t round = 0; round < roundCount; ++round) {
        std::array<BenchmarkResult, 5> roundResults{};
        std::array<std::size_t, 5> order{{0, 1, 2, 3, 4}};
        std::shuffle(order.begin(), order.end(), randomGenerator);
        for (const std::size_t configuration : order) {
            const bool sequential = configuration == 0;
            roundResults[configuration] =
                measure(sequential, workerCounts[configuration]);
        }
        rounds.push_back(std::move(roundResults));
    }

    std::vector<BenchmarkResult> results;
    results.reserve(workerCounts.size());
    for (std::size_t configuration = 0;
         configuration < workerCounts.size(); ++configuration) {
        BenchmarkResult summary;
        summary.sequential = configuration == 0;
        summary.workerCount = workerCounts[configuration];
        summary.roundsCompleted = roundCount;

        std::vector<double> durations;
        std::vector<double> payloadSizes;
        std::vector<double> throughputs;

        for (const auto& round : rounds) {
            const BenchmarkResult& sample = round[configuration];
            summary.successfulDownloads += sample.successfulDownloads;
            summary.failedDownloads += sample.failedDownloads;
            summary.peakSimultaneousDownloads = std::max(
                summary.peakSimultaneousDownloads,
                sample.peakSimultaneousDownloads);

            const BenchmarkResult& baseline = round[0];
            const bool baselineValid = baseline.failedDownloads == 0 &&
                baseline.successfulDownloads == urls.size() &&
                baseline.bytesDownloaded > 0 && baseline.elapsedSeconds > 0.0;
            const bool sampleValid = sample.failedDownloads == 0 &&
                sample.successfulDownloads == urls.size() &&
                sample.bytesDownloaded == baseline.bytesDownloaded &&
                sample.elapsedSeconds > 0.0;
            if (!baselineValid || !sampleValid) {
                continue;
            }

            ++summary.validRounds;
            durations.push_back(sample.elapsedSeconds);
            payloadSizes.push_back(static_cast<double>(sample.bytesDownloaded));
            throughputs.push_back(
                (static_cast<double>(sample.bytesDownloaded) / (1024.0 * 1024.0)) /
                sample.elapsedSeconds);
        }

        summary.elapsedSeconds = medianOf(std::move(durations));
        summary.bytesDownloaded = static_cast<std::uint64_t>(
            medianOf(std::move(payloadSizes)));
        summary.medianMebibytesPerSecond = medianOf(std::move(throughputs));
        results.push_back(summary);
    }

    const BenchmarkResult& baseline = results.front();
    const bool baselineComparable = baseline.validRounds == roundCount &&
        baseline.elapsedSeconds > 0.0;
    if (baselineComparable) {
        for (std::size_t i = 1; i < results.size(); ++i) {
            BenchmarkResult& result = results[i];
            if (result.validRounds != roundCount || result.elapsedSeconds <= 0.0) {
                continue;
            }
            result.medianSpeedup = baseline.elapsedSeconds / result.elapsedSeconds;
            result.medianTimeReductionPercent =
                (baseline.elapsedSeconds - result.elapsedSeconds) * 100.0 /
                baseline.elapsedSeconds;
        }
    }

    errorMessage.clear();
    return results;
}

void DownloadManager::updateProgress(std::size_t taskId, double downloaded, double total) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskId == 0 || taskId > tasks_.size()) {
        return;
    }
    DownloadTask& task = tasks_[taskId - 1];
    task.status = DownloadStatus::Running;
    if (total > 0.0) {
        task.progressPercent = std::min(100.0, downloaded * 100.0 / total);
    }
}

void DownloadManager::markFailed(std::size_t taskId, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskId == 0 || taskId > tasks_.size()) {
        return;
    }
    tasks_[taskId - 1].status = DownloadStatus::Failed;
    tasks_[taskId - 1].errorMessage = message;
    std::string databaseError;
    if (!updateDatabaseRecordLocked(tasks_[taskId - 1], databaseError)) {
        tasks_[taskId - 1].errorMessage += " (history update failed: " + databaseError + ")";
    }
}

bool DownloadManager::updateDatabaseRecordLocked(const DownloadTask& task,
                                                  std::string& errorMessage) {
    sqlite3_stmt* statement = nullptr;
    const char* updateSql =
        "UPDATE downloads SET status = ?, progress = ?, error_message = ?, "
        "updated_at = CURRENT_TIMESTAMP WHERE id = ?;";
    if (sqlite3_prepare_v2(database_, updateSql, -1, &statement, nullptr) != SQLITE_OK) {
        errorMessage = sqlite3_errmsg(database_);
        return false;
    }
    sqlite3_bind_text(statement, 1, statusToString(task.status), -1, SQLITE_STATIC);
    sqlite3_bind_double(statement, 2, task.progressPercent);
    sqlite3_bind_text(statement, 3, task.errorMessage.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, task.databaseId);
    const int result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
        errorMessage = sqlite3_errmsg(database_);
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

void DownloadManager::runDownload(std::size_t taskId, std::string url,
                                  std::string filename) {
    std::int64_t databaseId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[taskId - 1].status = DownloadStatus::Running;
        databaseId = tasks_[taskId - 1].databaseId;
        std::string databaseError;
        if (!updateDatabaseRecordLocked(tasks_[taskId - 1], databaseError)) {
            tasks_[taskId - 1].status = DownloadStatus::Failed;
            tasks_[taskId - 1].errorMessage =
                "Could not update history before download: " + databaseError;
            return;
        }
    }

    std::error_code filesystemError;
    std::filesystem::create_directories("downloads", filesystemError);
    if (filesystemError) {
        markFailed(taskId, "Could not create the downloads directory: " +
                               filesystemError.message());
        return;
    }

    const std::filesystem::path outputPath =
        std::filesystem::path("downloads") /
        (std::to_string(databaseId) + "_" + filename);
    TransferContext context{
        [this, taskId](double downloaded, double total) {
            updateProgress(taskId, downloaded, total);
        },
        std::ofstream(outputPath, std::ios::binary)
    };
    if (!context.output) {
        markFailed(taskId, "Could not open the output file for writing.");
        return;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::filesystem::remove(outputPath, filesystemError);
        markFailed(taskId, "Could not create a libcurl download handle.");
        return;
    }

    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MultiThread/0.1");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    context.output.close();

    if (result != CURLE_OK || !context.output) {
        const std::string message = errorBuffer[0] != '\0'
            ? errorBuffer
            : (result != CURLE_OK ? curl_easy_strerror(result)
                                  : "Writing the downloaded file failed.");
        std::filesystem::remove(outputPath, filesystemError);
        markFailed(taskId, message);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    DownloadTask& task = tasks_[taskId - 1];
    task.status = DownloadStatus::Completed;
    task.progressPercent = 100.0;
    std::string databaseError;
    if (!updateDatabaseRecordLocked(task, databaseError)) {
        task.errorMessage = "Download completed, but history update failed: " + databaseError;
    }
}
