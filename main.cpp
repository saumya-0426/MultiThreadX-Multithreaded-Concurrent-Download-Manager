#include "DownloadManager.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
void showTasks(const DownloadManager& manager) {
    const std::vector<DownloadTask> tasks = manager.getTasks();
    if (tasks.empty()) {
        std::cout << "No downloads have been added yet.\n";
        return;
    }

    std::cout << "\nID  STATUS   PROGRESS  FILENAME\n";
    for (const DownloadTask& task : tasks) {
        std::cout << task.id << "   " << statusToString(task.status) << "   "
                  << task.progressPercent << "%       " << task.filename << '\n'
                  << "    " << task.url << '\n';
        if (!task.errorMessage.empty()) {
            std::cout << "    Error: " << task.errorMessage << '\n';
        }
    }
    std::cout << "\nSaved files are written to the downloads folder.\n";
}

void showHistory(const DownloadManager& manager) {
    std::string errorMessage;
    const std::vector<DownloadHistoryEntry> history = manager.getHistory(errorMessage);
    if (!errorMessage.empty()) {
        std::cout << "Could not load download history: " << errorMessage << '\n';
        return;
    }
    if (history.empty()) {
        std::cout << "Download history is empty.\n";
        return;
    }

    std::cout << "\nID  STATUS     PROGRESS  FILE                  CREATED (UTC)\n";
    for (const DownloadHistoryEntry& entry : history) {
        std::cout << entry.id << "   " << statusToString(entry.status) << "   "
                  << entry.progressPercent << "%       " << entry.filename << "   "
                  << entry.createdAt << '\n'
                  << "    " << entry.url << '\n';
        if (!entry.errorMessage.empty()) {
            std::cout << "    Error: " << entry.errorMessage << '\n';
        }
    }
}

void runBenchmark(DownloadManager& manager) {
    constexpr std::size_t roundsToRun = 3;
    std::vector<std::string> urls;
    std::string url;
    std::cout << "Benchmark uses " << roundsToRun
              << " rounds; each URL is requested 15 times across all configurations.\n"
              << "Enter benchmark URLs, one per line. Submit a blank line to start.\n";
    while (std::getline(std::cin, url) && !url.empty()) {
        urls.push_back(url);
    }

    std::string errorMessage;
    const std::vector<BenchmarkResult> results =
        manager.benchmark(urls, roundsToRun, errorMessage);
    if (!errorMessage.empty()) {
        std::cout << "Could not run benchmark: " << errorMessage << '\n';
        return;
    }

    std::cout << "\nBenchmark results (medians across randomized rounds)\n";
    std::cout << std::left << std::setw(13) << "Mode/workers"
              << std::right << std::setw(9) << "OK total"
              << std::setw(8) << "Failed"
              << std::setw(9) << "Valid"
              << std::setw(8) << "Peak"
              << std::setw(11) << "MiB med"
              << std::setw(11) << "Sec med"
              << std::setw(11) << "MiB/s med"
              << std::setw(10) << "Time red%"
              << std::setw(10) << "Speedup" << '\n';

    for (const BenchmarkResult& result : results) {
        const bool comparable = results.front().validRounds ==
                results.front().roundsCompleted &&
            result.validRounds == result.roundsCompleted &&
            result.validRounds > 0;
        const double mebibytes =
            static_cast<double>(result.bytesDownloaded) / (1024.0 * 1024.0);
        const std::string mode = result.sequential
            ? "Sequential"
            : std::to_string(result.workerCount) + " workers";

        std::cout << std::left << std::setw(13) << mode
                  << std::right << std::setw(9) << result.successfulDownloads
                  << std::setw(8) << result.failedDownloads
                  << std::setw(4) << result.validRounds << '/'
                  << result.roundsCompleted
                  << std::setw(8) << result.peakSimultaneousDownloads
                  << std::setw(11) << std::fixed << std::setprecision(2) << mebibytes
                  << std::setw(11) << result.elapsedSeconds
                  << std::setw(11) << result.medianMebibytesPerSecond
                  << std::setw(10);
        if (!comparable || result.sequential) {
            std::cout << "n/a";
        } else {
            std::cout << std::setprecision(2)
                      << result.medianTimeReductionPercent << '%';
        }
        std::cout << std::setw(10);
        if (!comparable) {
            std::cout << "n/a";
        } else if (result.sequential) {
            std::cout << "baseline";
        } else {
            std::cout << std::setprecision(2) << result.medianSpeedup << 'x';
        }
        std::cout << '\n';
    }
    std::cout << "Benchmark transfers are discarded and do not enter download history.\n";
}
} // namespace

int main() {
    std::cout << "MultiThread - Concurrent Download Manager\n";
    DownloadManager manager;
    std::string choice;

    while (true) {
        std::cout << "\n1. Add download URL\n"
                  << "2. View downloads\n"
                  << "3. Benchmark downloads\n"
                  << "4. View download history\n"
                  << "5. Exit\n"
                  << "Choice: ";
        if (!std::getline(std::cin, choice)) {
            break;
        }

        if (choice == "1") {
            std::string url;
            std::cout << "URL: ";
            if (!std::getline(std::cin, url)) {
                break;
            }
            std::string errorMessage;
            if (manager.addUrl(url, errorMessage)) {
                std::cout << "Added as a pending task.\n";
            } else {
                std::cout << "Could not add URL: " << errorMessage << '\n';
            }
        } else if (choice == "2") {
            showTasks(manager);
        } else if (choice == "3") {
            runBenchmark(manager);
        } else if (choice == "4") {
            showHistory(manager);
        } else if (choice == "5") {
            std::cout << "Waiting for queued downloads to finish...\n";
            break;
        } else {
            std::cout << "Please choose 1, 2, 3, 4, or 5.\n";
        }
    }

    std::cout << "Goodbye.\n";
    return 0;
}
