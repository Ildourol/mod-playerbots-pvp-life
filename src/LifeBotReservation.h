/*
 * Shared bot reservations for Playerbots Life modules
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LIFE_BOT_RESERVATION_H
#define LIFE_BOT_RESERVATION_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace PlayerbotsLife
{
    enum class ReservationOwner : std::uint8_t
    {
        CityLife = 0,
        PvPLife  = 1,
        Artisans = 2
    };

    enum ModuleSystemFlags : std::uint32_t
    {
        MODULE_NONE       = 0x00,
        MODULE_CITY_LIFE  = 0x01,
        MODULE_PVP_LIFE   = 0x02,
        MODULE_ARTISANS   = 0x04
    };

    enum class LeaseState : std::uint8_t
    {
        Provisional = 0,
        Active      = 1,
        Releasing   = 2
    };

    struct LeaseToken
    {
        std::uint32_t GuidLow = 0;
        ReservationOwner Owner = ReservationOwner::CityLife;
        std::uint64_t ActivityId = 0;
        std::uint64_t Generation = 0;
        LeaseState State = LeaseState::Provisional;
    };

    struct DuelStagingSlot
    {
        std::uint32_t SlotId = 0;
        std::uint32_t MapId = 0;
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float CenterZ = 0.0f;
        std::uint64_t ActivityId = 0;
        ReservationOwner Owner = ReservationOwner::CityLife;
        std::uint32_t ExpiryTimeMs = 0;
    };

    struct CooperationSnapshot
    {
        std::uint32_t TotalReserved = 0;
        std::uint32_t CityLifeCount = 0;
        std::uint32_t PvPLifeCount = 0;
        std::uint32_t ArtisansCount = 0;
        std::uint32_t ProvisionalCount = 0;
        std::uint32_t ActiveCount = 0;
        std::uint32_t ReleasingCount = 0;
        std::uint32_t ActiveDuelSlots = 0;
        std::uint64_t CurrentGeneration = 0;
    };

    struct ReservationRegistry
    {
        std::mutex Mutex;
        std::unordered_map<std::uint32_t, ReservationOwner> Owners;
        std::unordered_map<std::uint32_t, LeaseToken> Leases;
        std::unordered_map<std::uint32_t, std::uint32_t> RealPlayerCooldowns;
        std::vector<DuelStagingSlot> ActiveDuelSlots;
        std::atomic<std::uint32_t> ActiveModules{MODULE_NONE};
        std::uint64_t NextGeneration{1};
        std::uint32_t StagingSlotIdCounter{1};
        std::uint32_t AdmissionRoundCounter{0};
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

    // -------------------------------------------------------------
    // Tokenized Lease Operations
    // -------------------------------------------------------------

    inline bool TryAcquireLease(LeaseToken& outToken, std::uint32_t guidLow, ReservationOwner owner,
                                std::uint64_t activityId, LeaseState initialState = LeaseState::Active)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        if (registry.Owners.find(guidLow) != registry.Owners.end())
            return false;

        std::uint64_t gen = ++registry.NextGeneration;
        outToken.GuidLow = guidLow;
        outToken.Owner = owner;
        outToken.ActivityId = activityId;
        outToken.Generation = gen;
        outToken.State = initialState;

        registry.Owners[guidLow] = owner;
        registry.Leases[guidLow] = outToken;
        return true;
    }

    inline bool TryAcquireBatch(std::vector<LeaseToken>& outTokens, std::vector<std::uint32_t> const& guids,
                                ReservationOwner owner, std::uint64_t activityId)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        for (std::uint32_t guidLow : guids)
        {
            if (registry.Owners.find(guidLow) != registry.Owners.end())
                return false;
        }

        outTokens.clear();
        outTokens.reserve(guids.size());
        for (std::uint32_t guidLow : guids)
        {
            std::uint64_t gen = ++registry.NextGeneration;
            LeaseToken token;
            token.GuidLow = guidLow;
            token.Owner = owner;
            token.ActivityId = activityId;
            token.Generation = gen;
            token.State = LeaseState::Active;

            registry.Owners[guidLow] = owner;
            registry.Leases[guidLow] = token;
            outTokens.push_back(token);
        }
        return true;
    }

    inline bool ValidateLease(LeaseToken const& token)
    {
        if (token.GuidLow == 0 || token.Generation == 0)
            return false;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.Leases.find(token.GuidLow);
        if (it == registry.Leases.end())
            return false;

        LeaseToken const& current = it->second;
        return (current.Owner == token.Owner &&
                current.Generation == token.Generation &&
                current.ActivityId == token.ActivityId &&
                current.State != LeaseState::Releasing);
    }

    inline void BeginRelease(LeaseToken const& token)
    {
        if (token.GuidLow == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.Leases.find(token.GuidLow);
        if (it != registry.Leases.end() &&
            it->second.Owner == token.Owner &&
            it->second.Generation == token.Generation)
        {
            it->second.State = LeaseState::Releasing;
        }
    }

    inline void ReleaseLease(LeaseToken const& token)
    {
        if (token.GuidLow == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.Leases.find(token.GuidLow);
        if (it != registry.Leases.end() &&
            it->second.Owner == token.Owner &&
            it->second.Generation == token.Generation)
        {
            registry.Leases.erase(it);
            registry.Owners.erase(token.GuidLow);
        }
    }

    inline bool PromoteLease(LeaseToken& token, std::uint64_t newActivityId = 0)
    {
        if (token.GuidLow == 0)
            return false;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.Leases.find(token.GuidLow);
        if (it == registry.Leases.end() ||
            it->second.Owner != token.Owner ||
            it->second.Generation != token.Generation)
        {
            return false;
        }

        it->second.State = LeaseState::Active;
        if (newActivityId != 0)
        {
            it->second.ActivityId = newActivityId;
            token.ActivityId = newActivityId;
        }
        token.State = LeaseState::Active;
        return true;
    }

    // -------------------------------------------------------------
    // Shared Duel Staging Lattice (20yd Spacing)
    // -------------------------------------------------------------

    inline bool ClaimDuelStagingSlot(std::uint32_t mapId, float reqX, float reqY, float reqZ,
                                     float maxRadius, std::uint64_t activityId, ReservationOwner owner,
                                     std::uint32_t nowMs, std::uint32_t durationMs, DuelStagingSlot& outSlot)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        // Prune expired slots
        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->ExpiryTimeMs != 0 && it->ExpiryTimeMs <= nowMs)
                it = slots.erase(it);
            else
                ++it;
        }

        // Search lattice candidates around reqX, reqY in 20-yard increments
        constexpr float SlotSpacing = 20.0f;
        constexpr float MinSeparation = 20.0f;
        int maxRings = static_cast<int>(maxRadius / SlotSpacing) + 1;

        for (int ring = 0; ring <= maxRings; ++ring)
        {
            for (int dx = -ring; dx <= ring; ++dx)
            {
                for (int dy = -ring; dy <= ring; ++dy)
                {
                    if (std::abs(dx) != ring && std::abs(dy) != ring)
                        continue;

                    float candX = reqX + static_cast<float>(dx) * SlotSpacing;
                    float candY = reqY + static_cast<float>(dy) * SlotSpacing;
                    float distFromReq = std::sqrt((candX - reqX) * (candX - reqX) + (candY - reqY) * (candY - reqY));
                    if (distFromReq > maxRadius)
                        continue;

                    bool collides = false;
                    for (DuelStagingSlot const& existing : slots)
                    {
                        if (existing.MapId != mapId)
                            continue;
                        float dz = std::abs(existing.CenterZ - reqZ);
                        float d = std::sqrt((existing.CenterX - candX) * (existing.CenterX - candX) +
                                            (existing.CenterY - candY) * (existing.CenterY - candY));
                        if (d < MinSeparation && dz <= 6.0f)
                        {
                            collides = true;
                            break;
                        }
                    }

                    if (!collides)
                    {
                        DuelStagingSlot slot;
                        slot.SlotId = ++registry.StagingSlotIdCounter;
                        slot.MapId = mapId;
                        slot.CenterX = candX;
                        slot.CenterY = candY;
                        slot.CenterZ = reqZ;
                        slot.ActivityId = activityId;
                        slot.Owner = owner;
                        slot.ExpiryTimeMs = nowMs + durationMs;

                        slots.push_back(slot);
                        outSlot = slot;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    inline void ReleaseDuelStagingSlot(std::uint32_t slotId)
    {
        if (slotId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->SlotId == slotId)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    inline void ReleaseDuelStagingSlotByActivity(std::uint64_t activityId)
    {
        if (activityId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->ActivityId == activityId)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    inline void ReleaseDuelStagingSlot(std::uint64_t activityId)
    {
        if (activityId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->ActivityId == activityId || it->SlotId == static_cast<std::uint32_t>(activityId))
                it = slots.erase(it);
            else
                ++it;
        }
    }

    // -------------------------------------------------------------
    // Shared Real Player Challenge Cooldown Ledger
    // -------------------------------------------------------------

    inline bool CanChallengeRealPlayer(std::uint32_t playerGuidLow, std::uint32_t nowSec, std::uint32_t cooldownSec = 0)
    {
        (void)cooldownSec;
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.RealPlayerCooldowns.find(playerGuidLow);
        if (it == registry.RealPlayerCooldowns.end() || nowSec >= it->second)
            return true;
        return false;
    }

    inline void RecordRealPlayerChallenge(std::uint32_t playerGuidLow, std::uint32_t nowSec, std::uint32_t cooldownSec)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        registry.RealPlayerCooldowns[playerGuidLow] = nowSec + cooldownSec;
    }

    // -------------------------------------------------------------
    // Fair Admission Round Arbitration
    // -------------------------------------------------------------

    inline ReservationOwner GetNextContestedPriorityOwner()
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        ++registry.AdmissionRoundCounter;
        return (registry.AdmissionRoundCounter % 2 == 0) ? ReservationOwner::CityLife : ReservationOwner::PvPLife;
    }

    // -------------------------------------------------------------
    // Truthful Diagnostics Snapshot
    // -------------------------------------------------------------

    inline CooperationSnapshot GetCooperationSnapshot()
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        CooperationSnapshot snap;
        snap.TotalReserved = static_cast<std::uint32_t>(registry.Owners.size());
        snap.ActiveDuelSlots = static_cast<std::uint32_t>(registry.ActiveDuelSlots.size());
        snap.CurrentGeneration = registry.NextGeneration;

        for (auto const& pair : registry.Owners)
        {
            if (pair.second == ReservationOwner::CityLife)
                ++snap.CityLifeCount;
            else if (pair.second == ReservationOwner::PvPLife)
                ++snap.PvPLifeCount;
            else if (pair.second == ReservationOwner::Artisans)
                ++snap.ArtisansCount;
        }

        for (auto const& pair : registry.Leases)
        {
            if (pair.second.State == LeaseState::Provisional)
                ++snap.ProvisionalCount;
            else if (pair.second.State == LeaseState::Active)
                ++snap.ActiveCount;
            else if (pair.second.State == LeaseState::Releasing)
                ++snap.ReleasingCount;
        }

        return snap;
    }

    // -------------------------------------------------------------
    // Legacy Compatibility API (Preserved for existing consumers)
    // -------------------------------------------------------------

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
        LeaseToken token;
        return TryAcquireLease(token, guidLow, owner, 0, LeaseState::Active);
    }

    inline void Release(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const entry = registry.Owners.find(guidLow);
        if (entry != registry.Owners.end() && entry->second == owner)
        {
            registry.Owners.erase(entry);
            registry.Leases.erase(guidLow);
        }
    }
}

#endif
