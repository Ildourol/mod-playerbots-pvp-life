/*
 * Shared bot reservations for Playerbots Life modules
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LIFE_BOT_RESERVATION_H
#define LIFE_BOT_RESERVATION_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace PlayerbotsLife
{
    enum class ReservationOwner : std::uint8_t
    {
        CityLife,
        PvPLife,
        Artisans
    };

    enum ModuleSystemFlags : std::uint32_t
    {
        MODULE_NONE       = 0x00,
        MODULE_CITY_LIFE  = 0x01,
        MODULE_PVP_LIFE   = 0x02,
        MODULE_ARTISANS   = 0x04
    };

    struct ReservationRegistry
    {
        std::mutex Mutex;
        std::unordered_map<std::uint32_t, ReservationOwner> Owners;
        std::atomic<std::uint32_t> ActiveModules{MODULE_NONE};
    };

    inline ReservationRegistry& GetReservationRegistry()
    {
        static ReservationRegistry registry;
        return registry;
    }

    inline void RegisterActiveModule(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        registry.ActiveModules.fetch_or(static_cast<std::uint32_t>(flag));
    }

    inline void UnregisterActiveModule(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        registry.ActiveModules.fetch_and(~static_cast<std::uint32_t>(flag));
    }

    inline bool IsModuleActive(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        return (registry.ActiveModules.load() & static_cast<std::uint32_t>(flag)) != 0;
    }

    inline std::uint32_t GetActiveModules()
    {
        return GetReservationRegistry().ActiveModules.load();
    }

    inline bool IsReserved(std::uint32_t guidLow)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        return registry.Owners.find(guidLow) != registry.Owners.end();
    }

    inline bool IsReservedBy(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const entry = registry.Owners.find(guidLow);
        return entry != registry.Owners.end() && entry->second == owner;
    }

    inline bool TryReserve(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const [entry, inserted] = registry.Owners.emplace(guidLow, owner);
        return inserted || entry->second == owner;
    }

    inline void Release(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const entry = registry.Owners.find(guidLow);
        if (entry != registry.Owners.end() && entry->second == owner)
            registry.Owners.erase(entry);
    }
}

#endif
