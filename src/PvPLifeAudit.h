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
            if (!_running.load(std::memory_order_acquire))
                return;

            _stopping.store(true, std::memory_order_release);
            _cv.notify_all();

            if (_workerThread.joinable())
                _workerThread.join();

            _running.store(false, std::memory_order_release);
            _stopping.store(false, std::memory_order_release);
        }

        ~AuditLogger()
        {
            Shutdown();
        }

    private:
        AuditLogger() = default;

        void StartWorker()
        {
            if (_running.load(std::memory_order_acquire))
                return;

            _stopping.store(false, std::memory_order_release);
            _running.store(true, std::memory_order_release);
            _workerThread = std::thread([this]() { WorkerLoop(); });
        }

        void WorkerLoop()
        {
            std::ofstream file;
            std::string currentPath;

            while (true)
            {
                std::queue<std::string> localQueue;
                std::string targetPath;
                bool pathChanged = false;
                {
                    std::unique_lock<std::mutex> lock(_queueMutex);
                    _cv.wait(lock, [this]() { return !_queue.empty() || _stopping.load(std::memory_order_acquire); });

                    if (_filePath != currentPath || !_fileReopened)
                    {
                        targetPath = _filePath;
                        pathChanged = true;
                        _fileReopened = true;
                    }

                    localQueue.swap(_queue);
                }

                if (pathChanged)
                {
                    if (file.is_open())
                        file.close();
                    currentPath = targetPath;
                    file.open(currentPath, std::ios::app);
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

                if (_stopping.load(std::memory_order_acquire))
                {
                    std::queue<std::string> residual;
                    {
                        std::lock_guard<std::mutex> lock(_queueMutex);
                        residual.swap(_queue);
                    }
                    if (file.is_open())
                    {
                        while (!residual.empty())
                        {
                            file << residual.front();
                            residual.pop();
                        }
                        file.flush();
                    }
                    break;
                }
            }

            if (file.is_open())
                file.close();
        }

        static constexpr size_t MaxQueueSize = 5000;
        std::atomic<bool> _enabled{false};
        std::atomic<bool> _running{false};
        std::atomic<bool> _stopping{false};
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
