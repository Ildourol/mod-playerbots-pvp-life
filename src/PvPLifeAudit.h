/*
 * PvP Life Audit Logger (Thread-Safe Asynchronous Bounded File Writer)
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVP_LIFE_AUDIT_H
#define PVP_LIFE_AUDIT_H

#include "GameTime.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace PvPLife
{
    class AuditLogger
    {
    public:
        static AuditLogger& Instance()
        {
            static AuditLogger instance;
            return instance;
        }

        void Configure(bool enabled, std::string const& filePath)
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            _enabled = enabled;
            if (_filePath != filePath)
            {
                _filePath = filePath;
                _fileReopened = false;
            }

            if (_enabled && !_running)
                StartWorker();
        }

        void Log(std::string const& tag, std::string const& message)
        {
            if (!_enabled)
                return;

            std::string line = std::to_string(GameTime::GetGameTime().count()) + " [" + tag + "] " + message + "\n";

            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                if (_queue.size() < MaxQueueSize)
                {
                    _queue.push(std::move(line));
                }
                else
                {
                    ++_droppedLines;
                }
            }
            _cv.notify_one();
        }

        void Shutdown()
        {
            if (!_running)
                return;

            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _stopping = true;
            }
            _cv.notify_all();

            if (_workerThread.joinable())
                _workerThread.join();

            _running = false;
            _stopping = false;
        }

        ~AuditLogger()
        {
            Shutdown();
        }

    private:
        AuditLogger() = default;

        void StartWorker()
        {
            if (_running)
                return;

            _running = true;
            _stopping = false;
            _workerThread = std::thread([this]() { WorkerLoop(); });
        }

        void WorkerLoop()
        {
            std::ofstream file;
            std::string currentPath;

            while (true)
            {
                std::queue<std::string> localQueue;
                {
                    std::unique_lock<std::mutex> lock(_queueMutex);
                    _cv.wait(lock, [this]() { return !_queue.empty() || _stopping; });

                    if (_filePath != currentPath || !_fileReopened)
                    {
                        if (file.is_open())
                            file.close();
                        currentPath = _filePath;
                        file.open(currentPath, std::ios::app);
                        _fileReopened = true;
                    }

                    localQueue.swap(_queue);
                }

                if (file.is_open())
                {
                    while (!localQueue.empty())
                    {
                        file << localQueue.front();
                        localQueue.pop();
                    }
                    file.flush();
                }

                if (_stopping)
                {
                    // Flush any remaining lines added right before stop
                    std::lock_guard<std::mutex> lock(_queueMutex);
                    while (!_queue.empty())
                    {
                        if (file.is_open())
                            file << _queue.front();
                        _queue.pop();
                    }
                    if (file.is_open())
                        file.flush();
                    break;
                }
            }

            if (file.is_open())
                file.close();
        }

        static constexpr size_t MaxQueueSize = 5000;
        std::atomic<bool> _enabled{false};
        std::atomic<bool> _running{false};
        bool _stopping{false};
        bool _fileReopened{false};
        std::string _filePath{"Logs/pvplife_audit.log"};

        std::mutex _queueMutex;
        std::condition_variable _cv;
        std::queue<std::string> _queue;
        std::atomic<uint32_t> _droppedLines{0};
        std::thread _workerThread;
    };
}

#define sPvPLifeAudit PvPLife::AuditLogger::Instance()

#endif
