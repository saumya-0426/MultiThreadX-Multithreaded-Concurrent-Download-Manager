# MultiThreadX

**A C++17 concurrent download manager with a custom thread pool, libcurl downloads, SQLite history, and a repeatable benchmark mode.**

## Introduction

Downloading files one at a time can leave the network idle while each request waits for a server. MultiThread queues HTTP/HTTPS URLs and lets a fixed-size group of worker threads process downloads concurrently.

The command-line app accepts URLs, shows task status and progress, saves completed files in `downloads/`, and records download history in `database/downloads.db`. Its benchmark compares a sequential baseline with thread pools configured for 1, 2, 4, and 8 workers.

## Features

- Fixed-size C++ thread pool and thread-safe FIFO task queue.
- Concurrent HTTP/HTTPS downloads using libcurl.
- Pending, running, completed, and failed task states.
- Progress reporting when the server provides content-length information.
- SQLite history with status, timestamps, and error details.
- Three-round benchmark with randomized configuration order and median results.
- Local automated self-tests for concurrency, downloads, history, and benchmark accounting.

## Core CS concepts

| Subject | Concepts demonstrated |
|---|---|
| Operating systems | Threads, producer-consumer pattern, mutexes, condition variables, worker lifecycle, synchronization, safe shutdown |
| Networking | HTTP/HTTPS requests, redirects, response codes, callbacks, progress, connection and server errors |
| Databases | SQLite schema, prepared statements, inserts, updates, queries, persistence, serialized access to a shared connection |
| Data structures | FIFO task queue, vectors of active tasks and history records |
| OOP and design | Encapsulation, task and manager abstractions, separation between CLI, scheduling, and transfer logic |
| Performance analysis | Sequential baseline, worker-count comparison, elapsed time, payload throughput, speedup, time reduction, peak concurrency |

## Tech stack

| Technology | What it does here |
|---|---|
| C++17 | Implements the CLI, task manager, thread pool, callbacks, filesystem handling, and timing |
| `std::thread`, `std::mutex`, `std::condition_variable` | Run workers and protect the shared task queue and download state |
| libcurl | Performs HTTP/HTTPS transfers, follows redirects, writes downloaded data, and reports progress and errors |
| SQLite | Persists download metadata and history between app runs |
| MSYS2 / MinGW-w64 GCC | Windows development environment and compiler; MSYS2 `pacman` installs the compiler and native development libraries |
| Winsock | Supplies the local HTTP fixture server used by `self_test.cpp` |

MSYS2 is a build and dependency environment, not part of the manager's task-scheduling design. The project uses PowerShell for its documented Windows build commands and does not use CMake.

## Project structure

```text
MultiThread/
|-- .gitignore
|-- README.md
|-- main.cpp
|-- DownloadTask.h
|-- DownloadManager.h
|-- DownloadManager.cpp
|-- ThreadPool.h
|-- ThreadPool.cpp
|-- self_test.cpp
|-- downloads/              # downloaded files created at runtime
`-- database/               # downloads.db created at runtime
```

| File or folder | Purpose |
|---|---|
| `main.cpp` | CLI menu for adding URLs, viewing active tasks, benchmarking, and viewing history |
| `DownloadTask.h` | Download states, task and history records, and benchmark result data structures |
| `DownloadManager.h/.cpp` | URL validation, task coordination, libcurl transfers, SQLite history, and benchmark runs |
| `ThreadPool.h/.cpp` | Fixed-size worker pool, FIFO queue, synchronization, wait-for-idle, and orderly shutdown |
| `self_test.cpp` | Standalone tests using a local loopback HTTP server and a temporary database |
| `.gitignore` | Excludes compiler outputs, downloaded files, and the runtime database |
| `downloads/` | Receives downloaded files; contents are generated locally and ignored by Git |
| `database/` | Holds `downloads.db`; generated locally and ignored by Git |

The normal download manager uses **3 worker threads** by default. Benchmark runs create temporary pools for the worker counts being measured.

## Requirements and dependency setup

- Windows with PowerShell.
- MSYS2 MINGW64 GCC with C++17 support.
- MSYS2 MINGW64 libcurl and SQLite development packages.

Open an MSYS2 MINGW64 terminal and install the packages:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3
```

The default MSYS2 install path used below is `C:\msys64`. If yours differs, update `$msys` in the PowerShell commands.

## Build and run

Run this in PowerShell from the project folder:

```powershell
$msys = "C:\msys64\mingw64"
$env:Path = "$msys\bin;$env:Path"
& "$msys\bin\g++.exe" -std=c++17 -pthread main.cpp ThreadPool.cpp DownloadManager.cpp -I "$msys\include" -L "$msys\lib" -lcurl -lsqlite3 -o MultiThread.exe
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
.\MultiThread.exe
```

Menu options:

1. Add a download URL.
2. View downloads in the current app session.
3. Benchmark sequential and concurrent transfers.
4. View persistent download history.
5. Exit and wait for queued downloads to finish.

Downloaded files are named with their history ID and saved under `downloads/`. The database is stored at `database/downloads.db`, relative to the app's current working directory.

## Self-tests

The test suite uses a loopback HTTP server and temporary files/database, so it does not require public test URLs or external network access. Build and run it from PowerShell in the project folder:

```powershell
$msys = "C:\msys64\mingw64"
$env:Path = "$msys\bin;$env:Path"
& "$msys\bin\g++.exe" -std=c++17 -pthread self_test.cpp ThreadPool.cpp DownloadManager.cpp -I "$msys\include" -L "$msys\lib" -lcurl -lsqlite3 -lws2_32 -o self_test.exe
if ($LASTEXITCODE -ne 0) { throw "Self-test build failed." }
.\self_test.exe
```

Expected result:

```text
All MultiThread self-tests passed.
```

The tests check that 64 thread-pool tasks each run once, concurrency stays within the worker limit, shutdown drains queued tasks, invalid schemes are rejected, successful and redirected downloads create files, 404 and connection failures are recorded, SQLite history survives a manager restart, and benchmark results correctly account for eight concurrent URLs and failures.

## Benchmark method

Choose **3. Benchmark downloads**, enter one or more direct HTTP/HTTPS URLs, and submit a blank line. The app runs three rounds. In each round it compares a sequential baseline with 1, 2, 4, and 8 workers in randomized order.

The benchmark discards response bodies instead of writing files or adding history. It reports median elapsed time and payload MiB/s, total successes and failures, peak simultaneous transfers, speedup, and time reduction. A comparison is considered valid only when all rounds succeed and the payload sizes match the sequential baseline. Each URL is requested 15 times per benchmark session: three rounds across five configurations.

The recorded benchmark used these four payload sizes:

```text
https://speed.cloudflare.com/__down?bytes=1000000
https://speed.cloudflare.com/__down?bytes=2000000
https://speed.cloudflare.com/__down?bytes=5000000
https://speed.cloudflare.com/__down?bytes=10000000
```

## Benchmark results

Three independent benchmark sessions were run on **2026-11-13** with the same four Cloudflare download endpoints, requesting 1 MB, 2 MB, 5 MB, and 10 MB payloads. Each session used three randomized rounds. Every configuration completed 12/12 transfers with 17.17 MiB received per configuration. The table shows the median of the three session medians; speedup and time reduction are calculated from those aggregate median times.

| Configuration | Median elapsed | Median throughput | Peak simultaneous | Speedup vs. sequential | Time reduction |
|---|---:|---:|---:|---:|---:|
| Sequential | 5.03 s | 3.41 MiB/s | 1 | Baseline | - |
| 1 worker | 4.92 s | 3.49 MiB/s | 1 | 1.02x | 2.2% |
| 2 workers | 4.88 s | 3.52 MiB/s | 2 | 1.03x | 3.0% |
| 4 workers | 4.99 s | 3.44 MiB/s | 4 | 1.01x | 0.8% |
| 8 workers | 4.97 s | 3.45 MiB/s | 4 | 1.01x | 1.2% |

### Time reduction vs. sequential

```text
Each # represents approximately one percentage point.

1 worker    2.2%  ##
2 workers   3.0%  ###
4 workers   0.8%  #
8 workers   1.2%  #
```

## Conclusion

On this four-URL workload, two workers had the lowest aggregate median: **4.88 s versus 5.03 s sequential**, about **1.03x speedup** or **3.0% lower elapsed time**. The difference is modest and did not hold in every individual session. Four and eight workers did not show a meaningful advantage; the eight-worker configuration reached only four simultaneous transfers because the test set contained four URLs.

These results demonstrate that the project measures concurrency and can compare configurations. They do **not** establish a general throughput improvement across networks, servers, or workloads. Use the measured values with their test conditions rather than claiming a broad speedup.

## Limitations and future work

- The regular download manager has a fixed default of three workers; it is not configurable from the menu.
- Benchmark results vary with network conditions, server limits, URL choice, and caching. The recorded benchmark uses one host and only four URLs.
- Benchmark bodies are discarded, so benchmark throughput does not include disk-write performance.
- Progress may remain at 0% when a server does not provide a usable content length.
- There is no retry policy, cancellation, pause/resume, priority scheduling, or parallel chunk download.
- History uses a database path relative to the current working directory; launch the app from the project folder to use the expected `database/downloads.db`.
