#ifndef MULTITHREAD_DOWNLOAD_TASK_H
#define MULTITHREAD_DOWNLOAD_TASK_H

#include <cstddef>
#include <cstdint>
#include <string>

enum class DownloadStatus {
    Pending,
    Running,
    Completed,
    Failed
};

struct DownloadTask {
    std::size_t id;
    std::int64_t databaseId = 0;
    std::string url;
    std::string filename;
    DownloadStatus status = DownloadStatus::Pending;
    double progressPercent = 0.0;
    std::string errorMessage;
};

struct DownloadHistoryEntry {
    std::int64_t id;
    std::string url;
    std::string filename;
    DownloadStatus status = DownloadStatus::Pending;
    double progressPercent = 0.0;
    std::string createdAt;
    std::string errorMessage;
};

struct BenchmarkResult {
    bool sequential = false;
    std::size_t workerCount = 0;
    std::size_t roundsCompleted = 0;
    std::size_t validRounds = 0;
    double elapsedSeconds = 0.0;
    std::uint64_t bytesDownloaded = 0;
    std::size_t successfulDownloads = 0;
    std::size_t failedDownloads = 0;
    std::size_t peakSimultaneousDownloads = 0;
    double medianMebibytesPerSecond = 0.0;
    double medianSpeedup = 0.0;
    double medianTimeReductionPercent = 0.0;
};

inline const char* statusToString(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Pending: return "Pending";
        case DownloadStatus::Running: return "Running";
        case DownloadStatus::Completed: return "Completed";
        case DownloadStatus::Failed: return "Failed";
    }
    return "Unknown";
}

#endif
