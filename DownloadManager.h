#ifndef MULTITHREAD_DOWNLOAD_MANAGER_H
#define MULTITHREAD_DOWNLOAD_MANAGER_H

#include "DownloadTask.h"

#include <mutex>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

class DownloadManager {
public:
    explicit DownloadManager(std::size_t workerCount = 3);
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    bool addUrl(const std::string& url, std::string& errorMessage);
    std::vector<DownloadTask> getTasks() const;
    std::vector<DownloadHistoryEntry> getHistory(std::string& errorMessage) const;
    std::vector<BenchmarkResult> benchmark(const std::vector<std::string>& urls,
                                           std::size_t roundCount,
                                           std::string& errorMessage);

private:
    static bool isHttpUrl(const std::string& url);
    static std::string filenameFromUrl(const std::string& url,
                                       std::size_t taskNumber);

    mutable std::mutex mutex_;
    std::vector<DownloadTask> tasks_;
    std::unique_ptr<class ThreadPool> pool_;
    sqlite3* database_ = nullptr;

    void runDownload(std::size_t taskId, std::string url, std::string filename);
    void updateProgress(std::size_t taskId, double downloaded, double total);
    void markFailed(std::size_t taskId, const std::string& message);
    bool updateDatabaseRecordLocked(const DownloadTask& task,
                                   std::string& errorMessage);
};

#endif
