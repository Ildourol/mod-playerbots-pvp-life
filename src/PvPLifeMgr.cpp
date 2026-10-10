/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PvPLifeMgr.h"
#include "PvPLifeDuelSelection.h"
#include "PvPLifeDuelRequestPolicy.h"
#include "PvPLifeAudit.h"
#include "LifeBotReservation.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <shared_mutex>

namespace
{
    inline bool IsLifeBotReserved(uint32 guidLow)
    {
        return PlayerbotsLife::IsReserved(guidLow);
    }

    inline bool TryReserveLifeBot(uint32 guidLow)
    {
        return PlayerbotsLife::TryReserve(guidLow, PlayerbotsLife::ReservationOwner::PvPLife);
    }

    inline void ReleaseLifeBot(uint32 guidLow)
    {
        PlayerbotsLife::Release(guidLow, PlayerbotsLife::ReservationOwner::PvPLife);
    }

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::vector<std::string> Tokenize(std::string const& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> out;
        std::string token;
        while (stream >> token)
            out.push_back(token);
        return out;
    }

    uint32 GameTimeSeconds()
    {
        return static_cast<uint32>(GameTime::GetGameTime().count());
    }

    char const* TypeName(PvPLife::ActivityType type)
    {
        switch (type)
        {
            case PvPLife::ActivityType::Skirmish: return "Skirmish";
            case PvPLife::ActivityType::Duel: return "Duel";
            case PvPLife::ActivityType::ForTheHorde: return "ForTheHorde";
            case PvPLife::ActivityType::ForTheAlliance: return "ForTheAlliance";
        }
        return "Unknown";
    }

    char const* TeamName(PvPLife::TeamSide team)
    {
        switch (team)
        {
            case PvPLife::TeamSide::Alliance: return "Alliance";
            case PvPLife::TeamSide::Horde: return "Horde";
            default: return "Any";
        }
    }

    bool ParseTeam(std::string token, PvPLife::TeamSide& out)
    {
        token = Lower(token);
        if (token == "a" || token == "ally" || token == "alliance")
        {
            out = PvPLife::TeamSide::Alliance;
            return true;
        }
        if (token == "h" || token == "horde")
        {
            out = PvPLife::TeamSide::Horde;
            return true;
        }
        if (token == "any" || token == "both")
        {
            out = PvPLife::TeamSide::Any;
            return true;
        }
        return false;
    }

    bool ParseType(std::string token, PvPLife::ActivityType& out)
    {
        token = Lower(token);
        if (token == "skirmish" || token == "world" || token == "worldpvp")
        {
            out = PvPLife::ActivityType::Skirmish;
            return true;
        }
        if (token == "duel")
        {
            out = PvPLife::ActivityType::Duel;
            return true;
        }
        if (token == "forthehorde" || token == "fth")
        {
            out = PvPLife::ActivityType::ForTheHorde;
            return true;
        }
        if (token == "forthealliance" || token == "fta" || token == "fortheally")
        {
            out = PvPLife::ActivityType::ForTheAlliance;
            return true;
        }
        return false;
    }
}

namespace PvPLife
{
    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
    }

    void Manager::AuditLog(std::string const& tag, std::string const& text) const
    {
        if (_auditEnable)
            sPvPLifeAudit.Log(tag, text);
    }

    void Manager::Shutdown()
    {
        for (ActiveEvent const& e : _activeEvents)
            EndEvent(e, "shutdown");
        _activeEvents.clear();
        _moveQueue.clear();
        _duelQueue.clear();
        _engagementQueue.clear();
        _chatQueue.clear();

        for (PendingRaidResponse const& p : _pendingResponses)
            CancelPendingRaidResponse(p.ResponseId, "shutdown");
        _pendingResponses.clear();

        sPvPLifeAudit.Shutdown();
    }

    void Manager::LoadConfig()
    {
        // 1. Master & Management Timing
        _enable = sConfigMgr->GetOption<bool>("PvPLife.Enable", true);
        _debug = sConfigMgr->GetOption<bool>("PvPLife.Debug", false);
        _announce = sConfigMgr->GetOption<bool>("PvPLife.Announce", false);
        _startupDelaySeconds = sConfigMgr->GetOption<uint32>("PvPLife.StartupDelaySeconds", 90);
        _tickSeconds = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("PvPLife.TickSeconds", 20));
        _maxActiveEvents = sConfigMgr->GetOption<uint32>("PvPLife.Events.MaxActive", 0);

        // 2. Bot Selection & Filtering
        _requireBotEligibility = sConfigMgr->GetOption<bool>("PvPLife.Bots.RequireBotEligibility", true);
        _usePlayerbotConfig = sConfigMgr->GetOption<bool>("PvPLife.Bots.UsePlayerbotConfig", true);
        _botAccountPrefix = sConfigMgr->GetOption<std::string>("PvPLife.Bots.AccountPrefix", "auto");
        _botAccountMin = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMin", 0);
        _botAccountMax = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMax", 0);
        std::string filterModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Bots.AccountFilterMode", "legacy_any"));
        _accountFilterMode = (filterModeStr == "all") ? AccountFilterMode::All : AccountFilterMode::LegacyAny;

        _botQueryLimit = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("PvPLife.Bots.QueryLimit", 500));
        _minOnlineRequired = sConfigMgr->GetOption<uint32>("PvPLife.Bots.MinOnlineRequired", 3);
        _globalMinLevel = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.GlobalMinLevel", 20), 1, 80));

        _skipGroupedBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.SkipGrouped", true);
        _respectPlayerbotActivity = sConfigMgr->GetOption<bool>("PvPLife.Bots.RespectPlayerbotActivity", false);
        _allowPartialTeams = sConfigMgr->GetOption<bool>("PvPLife.Bots.AllowPartialTeams", true);
        _minimumBotsPerSide = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.MinimumPerSide", 1), 1, 10);
        _maxBotsPerSide = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.MaxPerSide", 100), 1, 200);
        _minimumBotsPerSide = std::min(_minimumBotsPerSide, _maxBotsPerSide);
        _returnBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.ReturnAfterActivity", true);

        // 3. Movement & Combat
        _teleportOnArrival = sConfigMgr->GetOption<bool>("PvPLife.Movement.TeleportOnArrival", true);
        _useMovePoint = sConfigMgr->GetOption<bool>("PvPLife.Movement.UseMovePoint", true);
        _hardTeleportToTarget = sConfigMgr->GetOption<bool>("PvPLife.Movement.HardTeleportToTarget", false);
        _positionJitter = sConfigMgr->GetOption<uint32>("PvPLife.Movement.PositionJitter", 10);
        _arrivalStaggerMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMinSeconds", 2);
        _arrivalStaggerMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMaxSeconds", 12);
        _moveAfterArrivalMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMinSeconds", 6);
        _moveAfterArrivalMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMaxSeconds", 20);

        _strategiesEnable = sConfigMgr->GetOption<bool>("PvPLife.Strategies.Enable", true);
        _forcePvpFlag = sConfigMgr->GetOption<bool>("PvPLife.Combat.ForcePvpFlag", true);
        _forceAttackOnEngage = sConfigMgr->GetOption<bool>("PvPLife.Combat.ForceAttackOnEngage", true);
        _engageAfterMoveSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageAfterMoveSeconds", 4);
        _engageRetrySeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageRetrySeconds", 5));
        _engageMaxRetries = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageMaxRetries", 12), 1, 50);

        _pvpCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.Combat", "+pvp,+boost,+dps debuff,-passive,-stay");
        _pvpNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.NonCombat", "+pvp,+boost,-passive,-stay");
        _duelNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.DuelNonCombat", "+duel,+pvp,+boost,+stay,-follow,-passive,-grind");

        // 4. Persistent World PvP
        _alwaysActiveWorldPvp = sConfigMgr->GetOption<bool>("PvPLife.World.AlwaysActive", true);
        _minActiveSkirmishes = sConfigMgr->GetOption<uint32>("PvPLife.World.MinActiveHotspots", 1);
        _maxActiveSkirmishes = std::max(_minActiveSkirmishes, sConfigMgr->GetOption<uint32>("PvPLife.World.MaxActiveHotspots", 3));
        _randomSkirmishChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.World.RandomStartChance", 35));

        // 5. Duel Zones
        _duelEnable = sConfigMgr->GetOption<bool>("PvPLife.Duel.Enable", true);
        _duelAlwaysActive = sConfigMgr->GetOption<bool>("PvPLife.Duel.AlwaysActive", true);
        _duelSpellId = sConfigMgr->GetOption<uint32>("PvPLife.Duel.SpellId", 7266);
        _duelPairLimit = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairLimit", 12), 1, 40);
        _duelPairDelayMin = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMinSeconds", 5);
        _duelPairDelayMax = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMaxSeconds", 18);
        _duelLeashRadius = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("PvPLife.Duel.LeashRadius", 40));
        _duelGuardIntervalMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.GuardIntervalMs", 1000), 250, 5000);
        _challengeRealPlayers = sConfigMgr->GetOption<bool>("PvPLife.Duel.ChallengeRealPlayers", true);
        _realPlayerChallengeChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeChance", 25));
        _realPlayerScanRadius = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerScanRadius", 55);
        _realPlayerMaxLevelDifference = sConfigMgr->GetOption<uint32>("PvPLife.Duel.MaxLevelDifference", 5);
        _realPlayerCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeCooldownSeconds", 900);
        _botChallengeCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotChallengeCooldownSeconds", 180);
        _botDuelCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotCooldownSeconds", 45);
        _botDuelMaxLevelDifference = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotMaxLevelDifference", 5);

        // 6. Campaigns & Chatter
        _forTheHordeEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheHorde.Enable", true);
        _forTheAllianceEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheAlliance.Enable", true);
        _factionCampaignChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.ChancePerTick", 12));
        _factionCampaignGlobalCooldown = sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.GlobalCooldownSeconds", 3600);

        _botChatEnable = sConfigMgr->GetOption<bool>("PvPLife.BotChat.Enable", false);
        _botChatYellChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.YellChance", 0));
        _botChatWorldChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.WorldChance", 0));

        // 7. Reactive Raid Response
        _reactiveEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.Enable", true);
        _reactiveAllianceEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.AllianceEnable", true);
        _reactiveHordeEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.HordeEnable", true);
        _reactiveMinGroupSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MinGroupSize", 20), 2, 40);
        _reactiveMaxResponders = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxResponders", 40), 1, 40);
        std::string lvlModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.ReactiveRaidResponse.LevelMode", "raid"));
        _reactiveLevelMode = (lvlModeStr == "fixed") ? ResponderLevelMode::Fixed : ResponderLevelMode::Raid;
        _reactiveFixedLevel = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.FixedLevel", 60), 1, 80));
        _reactiveMaxLevelDifference = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxLevelDifference", 5), 0, 30));

        _reactiveLoginOfflineBots = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.LoginOfflineBots", false);
        _reactiveLoginDelaySeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.LoginDelaySeconds", 20));
        _reactiveLoginTimeoutSeconds = std::max<uint32>(_reactiveLoginDelaySeconds + 5, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.LoginTimeoutSeconds", 60));
        _reactiveMaxConcurrentLogins = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxConcurrentLogins", 4), 1, 20);
        _reactiveCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.CooldownSeconds", 1800);
        _reactiveDurationMinutes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.DurationMinutes", 20));
        _reactiveRallyDistance = std::max<float>(20.0f, sConfigMgr->GetOption<float>("PvPLife.ReactiveRaidResponse.RallyDistance", 120.0f));
        _reactiveLaneSpacing = std::max<float>(1.0f, sConfigMgr->GetOption<float>("PvPLife.ReactiveRaidResponse.LaneSpacing", 4.0f));
        _reactiveFormationColumns = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.FormationColumns", 9), 1, 15);
        _reactiveAdvanceDistance = std::max<float>(5.0f, sConfigMgr->GetOption<float>("PvPLife.ReactiveRaidResponse.AdvanceDistance", 25.0f));

        // 8. Population Borrowing Budget
        _populationBudgetEnable = sConfigMgr->GetOption<bool>("PvPLife.Population.Enable", true);
        std::string budgetModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Population.BudgetMode", "percentage"));
        _budgetMode = (budgetModeStr == "fixed") ? BudgetMode::Fixed : BudgetMode::Percentage;
        _fixedCount = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.FixedCount", 150), 2, 500);
        _borrowPercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.BorrowPercent", 12), 1, 100);
        _maxTotal = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.MaxTotal", 150), 2, 1000);
        std::string demandModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Population.DemandMode", "hybrid"));
        if (demandModeStr == "fixed")
            _demandMode = DemandMode::Fixed;
        else if (demandModeStr == "dynamic")
            _demandMode = DemandMode::Dynamic;
        else
            _demandMode = DemandMode::Hybrid;
        _baselinePercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.BaselinePercent", 20), 1, 100);
        _burstGraceSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Population.BurstGraceSeconds", 180);
        _maxChangesPerTick = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.MaxChangesPerTick", 3), 1, 20);

        // 9. Compatibility & Anti-Stealing Barrier
        _harmonyEnable = sConfigMgr->GetOption<bool>("PvPLife.Compatibility.CityLife.EnableHarmony", true);
        _respectCityLifeReservations = sConfigMgr->GetOption<bool>("PvPLife.Compatibility.CityLife.RespectReservations", true);
        _excludeCityLifeAccounts = sConfigMgr->GetOption<bool>("PvPLife.Compatibility.CityLife.ExcludeCityLifeAccounts", true);
        _independentHookProcessing = sConfigMgr->GetOption<bool>("PvPLife.Compatibility.CityLife.IndependentHookProcessing", true);

        // 10. Audit Logging
        _auditEnable = sConfigMgr->GetOption<bool>("PvPLife.Audit.Enable", true);
        _auditFilePath = sConfigMgr->GetOption<std::string>("PvPLife.Audit.FilePath", "Logs/pvplife_audit.log");
        sPvPLifeAudit.Configure(_auditEnable, _auditFilePath);

        // Sanity adjustments
        if (_arrivalStaggerMax < _arrivalStaggerMin)
            _arrivalStaggerMax = _arrivalStaggerMin;
        if (_moveAfterArrivalMax < _moveAfterArrivalMin)
            _moveAfterArrivalMax = _moveAfterArrivalMin;
        if (_duelPairDelayMax < _duelPairDelayMin)
            _duelPairDelayMax = _duelPairDelayMin;

        ImportPlayerbotDefaults();
        _botAccountCache.clear();
        _zoneConfig.clear();
        _startupElapsedMs = 0;
        _timerMs = 0;
        _duelGuardTimerMs = 0;

        if (!_requireBotEligibility)
        {
            LOG_ERROR("module", "[PvPLife] SAFETY CHECK FAILED: PvPLife.Bots.RequireBotEligibility must be enabled.");
            _enable = false;
        }

        _databaseReady = VerifyDatabase();
        if (_enable && !_databaseReady)
            _enable = false;
        if (_databaseReady)
            LoadZoneConfig();

        LOG_INFO("module", "[PvPLife] Master: enable={} DB={} alwaysWorld={} duel={} reactive={} budget={}({}%) "
            "prefix='{}' globalMinLvl={} respectReservations={}",
            _enable ? 1 : 0, _databaseReady ? 1 : 0, _alwaysActiveWorldPvp ? 1 : 0, _duelEnable ? 1 : 0,
            _reactiveEnable ? 1 : 0, _populationBudgetEnable ? 1 : 0, _borrowPercent,
            _botAccountPrefix, _globalMinLevel, _respectCityLifeReservations ? 1 : 0);

        AuditLog("CONFIG", "enabled=" + std::to_string(_enable ? 1 : 0) + " prefix=" + _botAccountPrefix +
            " globalMin=" + std::to_string(_globalMinLevel) + " budgetPercent=" + std::to_string(_borrowPercent) +
            " reactive=" + std::to_string(_reactiveEnable ? 1 : 0));
    }

    void Manager::ImportPlayerbotDefaults()
    {
        if (!_usePlayerbotConfig)
            return;

        std::string configuredPrefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot", false);
        uint32 loginDelay = sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30, false);
        std::string lowered = Lower(_botAccountPrefix);
        if (lowered.empty() || lowered == "auto" || lowered == "playerbot" || lowered == "playerbots")
            _botAccountPrefix = configuredPrefix;
        _startupDelaySeconds = std::max(_startupDelaySeconds, loginDelay);
    }

    bool Manager::VerifyDatabase()
    {
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp_life_zone'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_zone. Apply data/sql/manual/world_pvp_life.sql");
            return false;
        }
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp_life_chat'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_chat. Apply data/sql/manual/world_pvp_life.sql");
            return false;
        }
        return true;
    }

    std::string Manager::ZoneSelectSql(std::string const& suffix) const
    {
        return "SELECT id,name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
               "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o,"
               "attackers_min,attackers_max,defenders_min,defenders_max,duration_min,duration_max,weight,"
               "cooldown_seconds,last_start,challenge_players,bot_chat FROM pvp_life_zone " + suffix;
    }

    Zone Manager::ReadZone(Field* f)
    {
        Zone z;
        z.Id = f[0].Get<uint32>();
        z.Name = f[1].Get<std::string>();
        z.Enabled = f[2].Get<uint8>() != 0;
        z.Type = static_cast<ActivityType>(f[3].Get<uint8>());
        z.AttackerTeam = static_cast<TeamSide>(f[4].Get<uint8>());
        z.DefenderTeam = static_cast<TeamSide>(f[5].Get<uint8>());
        z.MinLevel = f[6].Get<uint8>();
        z.MaxLevel = f[7].Get<uint8>();
        z.MapId = f[8].Get<uint32>();
        z.RallyX = f[9].Get<float>(); z.RallyY = f[10].Get<float>(); z.RallyZ = f[11].Get<float>(); z.RallyO = f[12].Get<float>();
        z.TargetX = f[13].Get<float>(); z.TargetY = f[14].Get<float>(); z.TargetZ = f[15].Get<float>(); z.TargetO = f[16].Get<float>();
        z.AttackersMin = f[17].Get<uint32>(); z.AttackersMax = f[18].Get<uint32>();
        z.DefendersMin = f[19].Get<uint32>(); z.DefendersMax = f[20].Get<uint32>();
        z.PopulationMin = z.AttackersMin + z.DefendersMin;
        z.PopulationMax = z.AttackersMax + z.DefendersMax;
        z.DurationMin = f[21].Get<uint32>(); z.DurationMax = f[22].Get<uint32>();
        z.Weight = f[23].Get<uint32>(); z.CooldownSeconds = f[24].Get<uint32>(); z.LastStart = f[25].Get<uint32>();
        z.ChallengePlayers = f[26].Get<uint8>() != 0;
        z.BotChat = f[27].Get<uint8>() != 0;
        return z;
    }

    void Manager::LoadZoneConfig()
    {
        QueryResult result = WorldDatabase.Query(ZoneSelectSql("ORDER BY id").c_str());
        if (!result)
            return;

        uint32 minimumPopulation = _minimumBotsPerSide * 2;
        uint32 maximumPopulation = _maxBotsPerSide * 2;
        do
        {
            Zone zone = ReadZone(result->Fetch());
            ZoneConfig config;
            std::string prefix = "PvPLife.Zone." + zone.Name;
            config.Enabled = sConfigMgr->GetOption<bool>(prefix + ".Enable", zone.Enabled, false);
            config.MinPopulation = std::clamp<uint32>(
                sConfigMgr->GetOption<uint32>(prefix + ".MinPopulation", zone.PopulationMin, false),
                minimumPopulation, maximumPopulation);
            config.MaxPopulation = std::clamp<uint32>(
                sConfigMgr->GetOption<uint32>(prefix + ".MaxPopulation", zone.PopulationMax, false),
                config.MinPopulation, maximumPopulation);
            _zoneConfig[zone.Name] = config;
        } while (result->NextRow());
    }

    void Manager::ApplyZoneConfig(Zone& zone) const
    {
        auto it = _zoneConfig.find(zone.Name);
        if (it != _zoneConfig.end())
        {
            zone.Enabled = it->second.Enabled;
            zone.PopulationMin = it->second.MinPopulation;
            zone.PopulationMax = it->second.MaxPopulation;
        }
    }

    std::vector<Zone> Manager::LoadZones(bool readyOnly, int typeFilter)
    {
        std::vector<Zone> zones;
        std::string where = "WHERE 1=1";
        if (typeFilter >= 0)
            where += " AND activity_type=" + std::to_string(typeFilter);
        if (readyOnly)
        {
            uint32 now = GameTimeSeconds();
            where += " AND (last_start=0 OR last_start + cooldown_seconds <= " + std::to_string(now) + ")";
        }
        QueryResult result = WorldDatabase.Query(ZoneSelectSql(where + " ORDER BY weight DESC, id ASC").c_str());
        if (!result)
            return zones;
        do
        {
            Zone z = ReadZone(result->Fetch());
            ApplyZoneConfig(z);
            if (z.Weight > 0 && z.Enabled)
                zones.push_back(z);
        } while (result->NextRow());
        return zones;
    }

    bool Manager::LoadZoneByName(std::string name, Zone& out)
    {
        WorldDatabase.EscapeString(name);
        QueryResult result = WorldDatabase.Query(ZoneSelectSql("WHERE name='" + name + "' LIMIT 1").c_str());
        if (!result)
            return false;
        out = ReadZone(result->Fetch());
        ApplyZoneConfig(out);
        return true;
    }

    TeamSide Manager::TeamForRace(uint8 race) const
    {
        switch (race)
        {
            case RACE_HUMAN:
            case RACE_DWARF:
            case RACE_NIGHTELF:
            case RACE_GNOME:
            case RACE_DRAENEI:
                return TeamSide::Alliance;
            case RACE_ORC:
            case RACE_UNDEAD_PLAYER:
            case RACE_TAUREN:
            case RACE_TROLL:
            case RACE_BLOODELF:
                return TeamSide::Horde;
            default:
                return TeamSide::Any;
        }
    }

    Player* Manager::FindPlayer(uint32 guidLow) const
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
            return p;
        return ObjectAccessor::FindPlayer(guid);
    }

    bool Manager::IsConfiguredBotAccount(uint32 accountId)
    {
        if (!accountId)
            return false;
        auto cached = _botAccountCache.find(accountId);
        if (cached != _botAccountCache.end())
            return cached->second;

        bool hasRange = _botAccountMax > 0 && _botAccountMax >= _botAccountMin;
        bool rangeOk = hasRange && (accountId >= _botAccountMin && accountId <= _botAccountMax);
        bool hasPrefix = !_botAccountPrefix.empty();
        bool prefixOk = false;

        if (hasPrefix)
        {
            QueryResult result = LoginDatabase.Query("SELECT username FROM account WHERE id={} LIMIT 1", accountId);
            if (result)
            {
                std::string username = Lower(result->Fetch()[0].Get<std::string>());
                std::string prefix = Lower(_botAccountPrefix);
                prefixOk = username.rfind(prefix, 0) == 0;
            }
        }

        bool ok = false;
        if (_accountFilterMode == AccountFilterMode::All && hasRange && hasPrefix)
            ok = rangeOk && prefixOk;
        else
            ok = rangeOk || prefixOk;

        _botAccountCache[accountId] = ok;
        return ok;
    }

    bool Manager::IsRealPlayer(Player* player) const
    {
        if (!player || !player->GetSession())
            return false;
        return ::IsRealPlayer(player);
    }

    bool Manager::IsSafeBot(Player* player, bool allowPvPLifeReservation) const
    {
        if (!player || !player->GetSession() || !player->IsInWorld() || player->IsBeingTeleported())
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(player) &&
            !const_cast<Manager*>(this)->IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
            return false;

        uint32 guid = player->GetGUID().GetCounter();
        if (!allowPvPLifeReservation)
        {
            if (IsLifeBotReserved(guid))
                return false;
        }
        else
        {
            if (PlayerbotsLife::IsReservedBy(guid, PlayerbotsLife::ReservationOwner::CityLife))
                return false;
            if (PlayerbotsLife::IsReservedBy(guid, PlayerbotsLife::ReservationOwner::Artisans))
                return false;
        }

        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ai->HasGameClientMaster())
            return false;
        if (_respectPlayerbotActivity && !ai->AllowActivity(ALL_ACTIVITY))
            return false;
        if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->duel)
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue())
            return false;
        if (player->GetMap() && player->GetMap()->Instanceable())
            return false;
        if (_skipGroupedBots && player->GetGroup())
            return false;
        return true;
    }

    bool Manager::IsParticipant(uint32 guidLow) const
    {
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                if (p.Bot.GuidLow == guidLow)
                    return true;

        for (PendingRaidResponse const& r : _pendingResponses)
            for (BotCandidate const& b : r.Bots)
                if (b.GuidLow == guidLow)
                    return true;

        return false;
    }

    void Manager::DisengageParticipant(uint32 guidLow)
    {
        for (auto& event : _activeEvents)
        {
            auto it = std::find_if(event.Members.begin(), event.Members.end(),
                [guidLow](Participant const& p) { return p.Bot.GuidLow == guidLow; });
            if (it != event.Members.end())
            {
                if (it->DuelStagingSlotId != 0)
                    PlayerbotsLife::ReleaseDuelStagingSlot(it->DuelStagingSlotId);
                if (it->Token.GuidLow != 0)
                    PlayerbotsLife::ReleaseLease(it->Token);
                event.Members.erase(it);
                LOG_INFO("module", "[PvPLife] Disengaged participant {} from event #{}", guidLow, event.EventId);
                break;
            }
        }

        for (auto& pending : _pendingResponses)
        {
            auto it = std::find_if(pending.Bots.begin(), pending.Bots.end(),
                [guidLow](BotCandidate const& b) { return b.GuidLow == guidLow; });
            if (it != pending.Bots.end())
            {
                if (it->Token.GuidLow != 0)
                    PlayerbotsLife::ReleaseLease(it->Token);
                pending.Bots.erase(it);
                break;
            }
        }

        ReleaseLifeBot(guidLow);

        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(),
            [guidLow](MoveStep const& s) { return s.GuidLow == guidLow; }), _moveQueue.end());
        _duelQueue.erase(std::remove_if(_duelQueue.begin(), _duelQueue.end(),
            [guidLow](DuelStep const& s) { return s.ChallengerGuidLow == guidLow || s.TargetGuidLow == guidLow; }), _duelQueue.end());
        _engagementQueue.erase(std::remove_if(_engagementQueue.begin(), _engagementQueue.end(),
            [guidLow](EngagementStep const& s) { return s.BotGuidLow == guidLow || s.TargetGuidLow == guidLow; }), _engagementQueue.end());
        _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
            [guidLow](ChatStep const& s) { return s.SpeakerGuidLow == guidLow; }), _chatQueue.end());

        if (Player* p = FindPlayer(guidLow))
        {
            if (p->IsInCombat())
                p->CombatStop(true);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            {
                PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                PositionInfo stay = positions["stay"];
                stay.Reset();
                positions["stay"] = stay;
                ai->ResetStrategies();
            }
            if (sRandomPlayerbotMgr.IsRandomBot(p))
                sRandomPlayerbotMgr.ScheduleTeleport(guidLow);
        }
    }

    uint8 Manager::EffectiveMinLevel(uint8 minLevel) const
    {
        uint32 effective = std::max<uint32>(minLevel, _globalMinLevel);
        return static_cast<uint8>(std::min<uint32>(effective, 80));
    }

    std::vector<BotCandidate> Manager::LoadCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
        std::unordered_set<uint32> const& excluded)
    {
        std::vector<BotCandidate> candidates;
        uint8 effectiveMin = EffectiveMinLevel(minLevel);
        if (maxLevel < effectiveMin)
            return candidates;

        std::vector<uint32> onlineGuids;
        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            onlineGuids.reserve(players.size());
            for (auto const& pair : players)
                if (pair.second)
                    onlineGuids.push_back(pair.second->GetGUID().GetCounter());
        }

        for (uint32 guidLow : onlineGuids)
        {
            Player* player = FindPlayer(guidLow);
            if (!player || !player->GetSession() || player->GetLevel() < effectiveMin || player->GetLevel() > maxLevel)
                continue;

            BotCandidate b;
            b.GuidLow = guidLow;
            b.AccountId = player->GetSession()->GetAccountId();
            b.Name = player->GetName();
            b.Level = player->GetLevel();
            b.Race = player->getRace();
            b.Class = player->getClass();
            b.Team = TeamForRace(b.Race);
            b.Online = true;

            if (excluded.find(b.GuidLow) != excluded.end() || IsParticipant(b.GuidLow))
                continue;
            if (PlayerbotsLife::IsReserved(b.GuidLow))
                continue;
            if (team != TeamSide::Any && b.Team != team)
                continue;
            if (!IsSafeBot(player))
                continue;
            candidates.push_back(b);
        }

        for (size_t i = candidates.size(); i > 1; --i)
            std::swap(candidates[i - 1], candidates[urand(0, static_cast<uint32>(i - 1))]);
        if (candidates.size() > _botQueryLimit)
            candidates.resize(_botQueryLimit);

        return candidates;
    }

    std::vector<BotCandidate> Manager::LoadOfflineCandidates(TeamSide team, uint8 level,
        std::unordered_set<uint32> const& excluded)
    {
        std::vector<BotCandidate> candidates;
        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=0 AND level={} ORDER BY RAND() LIMIT {}", level, _botQueryLimit);
        if (!result)
            return candidates;

        do
        {
            Field* f = result->Fetch();
            BotCandidate b;
            b.GuidLow = f[0].Get<uint32>();
            b.AccountId = f[1].Get<uint32>();
            b.Name = f[2].Get<std::string>();
            b.Level = f[3].Get<uint8>();
            b.Race = f[4].Get<uint8>();
            b.Class = f[5].Get<uint8>();
            b.Online = false;
            b.Team = TeamForRace(b.Race);

            if (excluded.find(b.GuidLow) != excluded.end() || IsParticipant(b.GuidLow))
                continue;
            if (PlayerbotsLife::IsReserved(b.GuidLow))
                continue;
            if (team != TeamSide::Any && b.Team != team)
                continue;
            if (!IsConfiguredBotAccount(b.AccountId))
                continue;

            candidates.push_back(b);
        } while (result->NextRow());

        return candidates;
    }

    float Manager::Jitter() const
    {
        if (!_positionJitter)
            return 0.0f;
        return static_cast<float>(irand(-static_cast<int32>(_positionJitter), static_cast<int32>(_positionJitter)));
    }

    uint32 Manager::CalculatePopulationBudget() const
    {
        if (!_populationBudgetEnable)
            return _maxTotal;

        uint32 eligiblePool = 0;
        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& pair : players)
            {
                Player* p = pair.second;
                if (!p || !p->GetSession())
                    continue;
                uint32 guidLow = p->GetGUID().GetCounter();
                if (IsParticipant(guidLow) || PlayerbotsLife::IsReserved(guidLow))
                {
                    if (IsParticipant(guidLow))
                        ++eligiblePool;
                    continue;
                }
                if (sRandomPlayerbotMgr.IsRandomBot(p) || const_cast<Manager*>(this)->IsConfiguredBotAccount(p->GetSession()->GetAccountId()))
                {
                    PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
                    if (ai && !::IsRealPlayer(p) && !ai->HasGameClientMaster() && p->IsAlive())
                        ++eligiblePool;
                }
            }
        }

        uint32 budget = _fixedCount;
        if (_budgetMode == BudgetMode::Percentage)
            budget = (eligiblePool * _borrowPercent) / 100;

        return std::clamp<uint32>(budget, 2, std::min<uint32>(_maxTotal, eligiblePool ? eligiblePool : _maxTotal));
    }

    uint32 Manager::RandomCount(uint32 minCount, uint32 maxCount) const
    {
        minCount = std::min(minCount, _maxBotsPerSide);
        maxCount = std::min(maxCount, _maxBotsPerSide);
        if (maxCount < minCount)
            maxCount = minCount;
        return urand(minCount, maxCount);
    }

    void Manager::CalculatePopulation(Zone const& zone, uint32& attackerCount, uint32& defenderCount) const
    {
        uint32 minimumPopulation = std::clamp<uint32>(zone.PopulationMin, 2, _maxBotsPerSide * 2);
        uint32 maximumPopulation = std::clamp(zone.PopulationMax, minimumPopulation, _maxBotsPerSide * 2);
        uint32 totalPopulation = urand(minimumPopulation, maximumPopulation);

        uint32 budget = CalculatePopulationBudget();
        if (totalPopulation > budget)
            totalPopulation = std::max<uint32>(2, budget);

        uint64 attackerWeight = static_cast<uint64>(zone.AttackersMin) + zone.AttackersMax;
        uint64 defenderWeight = static_cast<uint64>(zone.DefendersMin) + zone.DefendersMax;
        uint64 totalWeight = attackerWeight + defenderWeight;
        if (!totalWeight)
        {
            attackerWeight = 1;
            totalWeight = 2;
        }

        attackerCount = static_cast<uint32>(
            (static_cast<uint64>(totalPopulation) * attackerWeight + totalWeight / 2) / totalWeight);
        attackerCount = std::clamp<uint32>(attackerCount, 1, totalPopulation - 1);
        defenderCount = totalPopulation - attackerCount;

        if (attackerCount > _maxBotsPerSide)
        {
            attackerCount = _maxBotsPerSide;
            defenderCount = totalPopulation - attackerCount;
        }
        if (defenderCount > _maxBotsPerSide)
        {
            defenderCount = _maxBotsPerSide;
            attackerCount = totalPopulation - defenderCount;
        }
    }

    void Manager::ApplyPvpStrategies(Player* player, bool duelMode) const
    {
        if (!player)
            return;
        if (_forcePvpFlag)
            player->SetPvP(true);
        if (_strategiesEnable)
        {
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            {
                ai->ChangeStrategy(_pvpCombatStrategies, BOT_STATE_COMBAT);
                ai->ChangeStrategy(duelMode ? _duelNonCombatStrategies : _pvpNonCombatStrategies, BOT_STATE_NON_COMBAT);
                if (duelMode)
                    ai->ChangeStrategy("+stay,-follow,-passive,-grind", BOT_STATE_NON_COMBAT);
            }
        }
    }

    void Manager::ScheduleMove(uint64 eventId, uint32 guidLow, std::string const& name, uint32 mapId,
        float x, float y, float z, float o, uint32 delaySeconds, bool teleport)
    {
        MoveStep step;
        step.EventId = eventId;
        step.GuidLow = guidLow;
        step.Name = name;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.MapId = mapId;
        step.X = x; step.Y = y; step.Z = z; step.O = o;
        step.Teleport = teleport;
        _moveQueue.push_back(step);
    }

    void Manager::ScheduleDuel(uint64 eventId, uint32 challengerGuid, uint32 targetGuid,
        std::string const& challengerName, std::string const& targetName, uint32 delaySeconds,
        bool realPlayerTarget)
    {
        if (!challengerGuid || !targetGuid || challengerGuid == targetGuid)
            return;
        std::set<std::pair<uint32, uint32>> busyPairs;
        for (DuelStep const& queued : _duelQueue)
        {
            if (queued.ChallengerGuidLow == challengerGuid || queued.TargetGuidLow == challengerGuid ||
                queued.ChallengerGuidLow == targetGuid || queued.TargetGuidLow == targetGuid)
                return;
            if (queued.EventId == eventId)
                busyPairs.emplace(std::min(queued.ChallengerGuidLow, queued.TargetGuidLow),
                    std::max(queued.ChallengerGuidLow, queued.TargetGuidLow));
        }
        for (ActiveEvent const& event : _activeEvents)
            if (event.EventId == eventId)
                for (Participant const& participant : event.Members)
                    if (Player* player = FindPlayer(participant.Bot.GuidLow); player && player->duel && player->duel->Opponent)
                    {
                        uint32 a = player->GetGUID().GetCounter();
                        uint32 b = player->duel->Opponent->GetGUID().GetCounter();
                        busyPairs.emplace(std::min(a, b), std::max(a, b));
                    }
        if (busyPairs.size() >= _duelPairLimit)
            return;

        DuelStep step;
        step.EventId = eventId;
        step.ChallengerGuidLow = challengerGuid;
        step.TargetGuidLow = targetGuid;
        step.ChallengerName = challengerName;
        step.TargetName = targetName;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.TargetIsRealPlayer = realPlayerTarget;
        _duelQueue.push_back(step);
    }

    void Manager::ScheduleEngagement(uint64 eventId, uint32 botGuid, uint32 targetGuid, uint32 delaySeconds)
    {
        EngagementStep step;
        step.EventId = eventId;
        step.BotGuidLow = botGuid;
        step.TargetGuidLow = targetGuid;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.RetryCount = 0;
        step.MaxRetries = static_cast<uint8>(_engageMaxRetries);
        _engagementQueue.push_back(step);
    }

    void Manager::MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport) const
    {
        if (!player)
            return;
        if (player->IsInCombat())
            player->CombatStop(true);

        if (teleport || player->GetMapId() != mapId || !_useMovePoint)
            player->TeleportTo(mapId, x, y, z, o);
        else
            player->GetMotionMaster()->MovePoint(0, x, y, z);
    }

    void Manager::EngageTarget(Player* bot, Player* target)
    {
        if (!bot || !target || !bot->IsAlive() || !target->IsAlive())
            return;
        if (bot->GetMapId() != target->GetMapId())
            return;

        if (_forcePvpFlag)
            bot->SetPvP(true);

        if (_strategiesEnable)
            ApplyPvpStrategies(bot, false);

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            ai->ChangeEngine(BOT_STATE_COMBAT);
            bot->Attack(target, true);
        }
    }

    bool Manager::StartZone(Zone const& zone, ChatHandler* handler, bool forced)
    {
        if (!zone.Enabled && !forced)
            return false;
        if (zone.Type == ActivityType::Duel && !_duelEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheHorde && !_forTheHordeEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheAlliance && !_forTheAllianceEnable && !forced)
            return false;

        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents && !forced)
            return false;

        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Id == zone.Id)
                return false;

        std::unordered_set<uint32> excluded;
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                excluded.insert(p.Bot.GuidLow);

        uint32 attackerNeed = 0;
        uint32 defenderNeed = 0;
        CalculatePopulation(zone, attackerNeed, defenderNeed);
        std::vector<BotCandidate> attackerPool =
            LoadCandidates(zone.AttackerTeam, zone.MinLevel, zone.MaxLevel, excluded);
        std::vector<BotCandidate> defenderPool =
            LoadCandidates(zone.DefenderTeam, zone.MinLevel, zone.MaxLevel, excluded);
        std::unordered_set<uint32> attackerIds;
        std::unordered_set<uint32> defenderIds;
        for (BotCandidate const& bot : attackerPool)
            attackerIds.insert(bot.GuidLow);
        for (BotCandidate const& bot : defenderPool)
            defenderIds.insert(bot.GuidLow);

        std::vector<BotCandidate> attackers;
        std::vector<BotCandidate> defenders;
        std::vector<BotCandidate> shared;
        for (BotCandidate const& bot : attackerPool)
        {
            if (defenderIds.count(bot.GuidLow))
                shared.push_back(bot);
            else if (attackers.size() < attackerNeed)
                attackers.push_back(bot);
        }
        for (BotCandidate const& bot : defenderPool)
            if (!attackerIds.count(bot.GuidLow) && defenders.size() < defenderNeed)
                defenders.push_back(bot);

        for (BotCandidate const& bot : shared)
        {
            bool attackerNeedsBot = attackers.size() < attackerNeed;
            bool defenderNeedsBot = defenders.size() < defenderNeed;
            if (!attackerNeedsBot && !defenderNeedsBot)
                break;

            if (attackerNeedsBot && (!defenderNeedsBot || attackers.size() <= defenders.size()))
                attackers.push_back(bot);
            else
                defenders.push_back(bot);
        }

        if (zone.Type == ActivityType::Duel)
        {
            auto pairs = SelectDiverseDuelPairs(attackerPool, defenderPool,
                std::min(attackerNeed, defenderNeed), _botDuelMaxLevelDifference);
            attackers = std::move(pairs.first);
            defenders = std::move(pairs.second);
        }

        uint32 requiredAttackers = _allowPartialTeams ? _minimumBotsPerSide : attackerNeed;
        uint32 requiredDefenders = _allowPartialTeams ? _minimumBotsPerSide : defenderNeed;
        if (attackers.size() < requiredAttackers || defenders.size() < requiredDefenders ||
            (attackers.size() + defenders.size()) < _minOnlineRequired)
        {
            if (_debug)
                LOG_INFO("module", "[PvPLife] '{}' lacks bots: attackers {}/{} defenders {}/{} total {} (min required {})",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed,
                    attackers.size() + defenders.size(), _minOnlineRequired);
            if (handler)
                handler->PSendSysMessage("PvP Life: Insufficient eligible bots for {} (attackers {}/{}, defenders {}/{}).",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed);
            return false;
        }

        uint32 now = GameTimeSeconds();
        ActiveEvent event;
        event.EventId = _nextEventId++;
        event.EventZone = zone;
        event.Origin = EventOrigin::Zone;
        event.StartedAt = now;
        uint32 dmin = std::max<uint32>(1, zone.DurationMin);
        uint32 dmax = std::max(dmin, zone.DurationMax);
        event.EndsAt = now + urand(dmin, dmax) * MINUTE;

        auto add = [&](BotCandidate const& bot, bool attacker, uint32 index)
        {
            Player* player = FindPlayer(bot.GuidLow);
            if (!IsSafeBot(player))
                return;

            Participant part;
            if (!PlayerbotsLife::TryAcquireLease(part.Token, bot.GuidLow, PlayerbotsLife::ReservationOwner::PvPLife, event.EventId, PlayerbotsLife::LeaseState::Active))
                return;

            part.Bot = bot;
            part.Attacker = attacker;
            part.OriginalMap = player->GetMapId();
            part.OriginalX = player->GetPositionX();
            part.OriginalY = player->GetPositionY();
            part.OriginalZ = player->GetPositionZ();
            part.OriginalO = player->GetOrientation();

            ApplyPvpStrategies(player, zone.Type == ActivityType::Duel);
            uint32 arrival = urand(_arrivalStaggerMin, _arrivalStaggerMax) + index;
            float sx, sy, sz, so;
            if (zone.Type == ActivityType::Duel)
            {
                uint32 duelJitter = std::min<uint32>(_positionJitter, 1);
                float sideOffset = attacker ? -6.0f : 6.0f;
                float laneOffset = (float(index) - float(std::max(attackers.size(), defenders.size()) - 1) * 0.5f) * 8.0f;
                sx = (zone.RallyX + zone.TargetX) * 0.5f + sideOffset +
                    static_cast<float>(irand(-static_cast<int32>(duelJitter), static_cast<int32>(duelJitter)));
                sy = (zone.RallyY + zone.TargetY) * 0.5f + laneOffset +
                    static_cast<float>(irand(-static_cast<int32>(duelJitter), static_cast<int32>(duelJitter)));
                sz = (zone.RallyZ + zone.TargetZ) * 0.5f;
                so = attacker ? zone.RallyO : zone.TargetO;

                PlayerbotsLife::DuelStagingSlot slot;
                uint32 eventDurationMs = (event.EndsAt > now ? (event.EndsAt - now) : 600) * 1000;
                if (PlayerbotsLife::ClaimDuelStagingSlot(zone.MapId, sx, sy, sz, 30.0f, event.EventId,
                    PlayerbotsLife::ReservationOwner::PvPLife, GameTimeMS(), eventDurationMs, slot))
                {
                    part.DuelStagingSlotId = slot.SlotId;
                    sx = slot.CenterX;
                    sy = slot.CenterY;
                }
            }
            else
            {
                sx = (attacker ? zone.RallyX : zone.TargetX) + Jitter();
                sy = (attacker ? zone.RallyY : zone.TargetY) + Jitter();
                sz = attacker ? zone.RallyZ : zone.TargetZ;
                so = attacker ? zone.RallyO : zone.TargetO;
            }
            part.ActivityX = sx;
            part.ActivityY = sy;
            part.ActivityZ = sz;
            part.ActivityO = so;
            event.Members.push_back(part);

            if (sRandomPlayerbotMgr.IsRandomBot(player))
                sRandomPlayerbotMgr.ScheduleTeleport(bot.GuidLow, event.EndsAt - now + 60);

            ScheduleMove(event.EventId, bot.GuidLow, bot.Name, zone.MapId, sx, sy, sz, so, arrival, _teleportOnArrival);

            AuditLog("MEMBER", "event=" + std::to_string(event.EventId) + " side=" + (attacker ? "attacker" : "defender") +
                " name=" + bot.Name + " guid=" + std::to_string(bot.GuidLow) + " lvl=" + std::to_string(bot.Level) + " class=" + std::to_string(bot.Class));

            if (zone.Type != ActivityType::Duel && attacker)
            {
                uint32 moveDelay = arrival + urand(_moveAfterArrivalMin, _moveAfterArrivalMax);
                ScheduleMove(event.EventId, bot.GuidLow, bot.Name, zone.MapId,
                    zone.TargetX + Jitter(), zone.TargetY + Jitter(), zone.TargetZ, zone.TargetO, moveDelay, _hardTeleportToTarget);

                if (_forceAttackOnEngage && !defenders.empty())
                {
                    uint32 targetGuid = defenders[urand(0, static_cast<uint32>(defenders.size() - 1))].GuidLow;
                    ScheduleEngagement(event.EventId, bot.GuidLow, targetGuid, moveDelay + _engageAfterMoveSeconds);
                }
            }
        };

        uint32 idx = 0;
        for (BotCandidate const& b : attackers)
            add(b, true, idx++);
        idx = 0;
        for (BotCandidate const& b : defenders)
            add(b, false, idx++);

        if (zone.Type == ActivityType::Duel)
        {
            uint32 base = _arrivalStaggerMax + 3;
            uint32 pairs = 0;
            std::unordered_set<uint32> usedDefenders;
            for (BotCandidate const& attacker : attackers)
            {
                if (pairs >= _duelPairLimit)
                    break;
                for (BotCandidate const& defender : defenders)
                {
                    if (usedDefenders.count(defender.GuidLow))
                        continue;
                    uint32 levelDiff = std::abs(static_cast<int32>(attacker.Level) - static_cast<int32>(defender.Level));
                    if (levelDiff > _botDuelMaxLevelDifference)
                        continue;
                    ScheduleDuel(event.EventId, attacker.GuidLow, defender.GuidLow, attacker.Name, defender.Name,
                        base + urand(_duelPairDelayMin, _duelPairDelayMax) + pairs * 2);
                    usedDefenders.insert(defender.GuidLow);
                    ++pairs;
                    break;
                }
            }
            AuditLog("DUEL_QUEUE", "event=" + std::to_string(event.EventId) + " spot=" + zone.Name + " pairs=" + std::to_string(pairs));
        }

        _activeEvents.push_back(event);
        WorldDatabase.Execute("UPDATE pvp_life_zone SET last_start={} WHERE id={}", now, zone.Id);

        if (zone.BotChat && _botChatEnable)
            ScheduleEventChat(_activeEvents.back());

        std::string label = std::string(TypeName(zone.Type)) + " at " + zone.Name;
        LOG_INFO("module", "[PvPLife] START #{} {} attackers={} defenders={} duration={}m",
            event.EventId, label, attackers.size(), defenders.size(), (event.EndsAt - event.StartedAt) / MINUTE);

        AuditLog("START", "event=" + std::to_string(event.EventId) + " spot=" + zone.Name +
            " mode=" + (zone.Type == ActivityType::Duel ? "duel" : "world_pvp") +
            " attackerTeam=" + TeamName(zone.AttackerTeam) + " defenderTeam=" + TeamName(zone.DefenderTeam) +
            " attackers=" + std::to_string(attackers.size()) + " defenders=" + std::to_string(defenders.size()) +
            " durationSeconds=" + std::to_string(event.EndsAt - event.StartedAt));

        if (_announce)
            Announce("[World PvP] " + label);
        if (handler)
            handler->PSendSysMessage("PvP Life: #{} {} started (bots {} + {}).",
                event.EventId, label, attackers.size(), defenders.size());
        return true;
    }

    void Manager::EndEvent(ActiveEvent const& event, std::string const& reason)
    {
        LOG_INFO("module", "[PvPLife] END #{} {} reason={}", event.EventId, event.EventZone.Name, reason);
        AuditLog("END", "event=" + std::to_string(event.EventId) + " spot=" + event.EventZone.Name +
            " reason=" + reason + " members=" + std::to_string(event.Members.size()));

        for (Participant const& part : event.Members)
        {
            if (part.DuelStagingSlotId != 0)
                PlayerbotsLife::ReleaseDuelStagingSlot(part.DuelStagingSlotId);

            if (part.Token.GuidLow != 0)
                PlayerbotsLife::BeginRelease(const_cast<PlayerbotsLife::LeaseToken&>(part.Token));
            else
                ReleaseLifeBot(part.Bot.GuidLow);

            Player* p = FindPlayer(part.Bot.GuidLow);
            if (p && p->GetSession() && !p->IsBeingTeleported())
            {
                if (p->IsInCombat())
                    p->CombatStop(true);
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                {
                    PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                    PositionInfo stay = positions["stay"];
                    stay.Reset();
                    positions["stay"] = stay;
                    ai->ResetStrategies();
                }
                if (sRandomPlayerbotMgr.IsRandomBot(p))
                    sRandomPlayerbotMgr.ScheduleTeleport(part.Bot.GuidLow);
                if (_returnBots)
                {
                    p->TeleportTo(part.OriginalMap, part.OriginalX, part.OriginalY, part.OriginalZ, part.OriginalO);
                    AuditLog("RETURN", "event=" + std::to_string(event.EventId) + " player=" + part.Bot.Name + " map=" + std::to_string(part.OriginalMap));
                }
            }

            if (part.Token.GuidLow != 0)
                PlayerbotsLife::ReleaseLease(const_cast<PlayerbotsLife::LeaseToken&>(part.Token));
        }
    }

    void Manager::ExpireEvents()
    {
        uint32 now = GameTimeSeconds();
        std::vector<ActiveEvent> keep;
        for (ActiveEvent const& e : _activeEvents)
        {
            bool lacksDuelPopulation = false;
            if (e.EventZone.Type == ActivityType::Duel && e.StartedAt + 30 <= now)
            {
                uint32 onlineMembers = 0;
                for (Participant const& participant : e.Members)
                {
                    Player* player = FindPlayer(participant.Bot.GuidLow);
                    if (player && player->IsInWorld() && player->IsAlive())
                        ++onlineMembers;
                }
                lacksDuelPopulation = onlineMembers < 2;
            }

            if (lacksDuelPopulation)
                EndEvent(e, "duel-population");
            else if (e.EndsAt <= now)
                EndEvent(e, "timer");
            else
                keep.push_back(e);
        }
        _activeEvents.swap(keep);
    }

    void Manager::ProcessMoveQueue()
    {
        uint32 now = GameTimeSeconds();
        std::vector<MoveStep> keep;
        for (MoveStep const& step : _moveQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            Player* p = FindPlayer(step.GuidLow);
            if (!p || p->IsBeingTeleported())
            {
                if (step.RetryCount < 4 && (p || step.RetryCount < 2))
                {
                    MoveStep retry = step;
                    retry.ExecuteAt = now + 2;
                    ++retry.RetryCount;
                    keep.push_back(retry);
                }
                continue;
            }
            MoveBot(p, step.MapId, step.X, step.Y, step.Z, step.O, step.Teleport);
            AuditLog("MOVE", "event=" + std::to_string(step.EventId) + " player=" + step.Name +
                " guid=" + std::to_string(step.GuidLow) + " map=" + std::to_string(step.MapId) +
                " x=" + std::to_string(step.X) + " y=" + std::to_string(step.Y) + " z=" + std::to_string(step.Z) +
                " mode=" + (step.Teleport ? "teleport" : "movepoint"));
        }
        _moveQueue.swap(keep);
    }

    void Manager::ProcessEngagementQueue()
    {
        if (_engagementQueue.empty())
            return;

        uint32 now = GameTimeSeconds();
        std::vector<EngagementStep> keep;
        for (EngagementStep const& step : _engagementQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            Player* bot = FindPlayer(step.BotGuidLow);
            Player* target = FindPlayer(step.TargetGuidLow);
            if (!bot || !target || !bot->IsAlive() || !target->IsAlive())
                continue;

            EngageTarget(bot, target);

            if (!bot->IsInCombat() && step.RetryCount < step.MaxRetries)
            {
                EngagementStep retry = step;
                retry.ExecuteAt = now + _engageRetrySeconds;
                ++retry.RetryCount;
                keep.push_back(retry);
            }
        }
        _engagementQueue.swap(keep);
    }

    void Manager::ProcessDuelQueue()
    {
        uint32 now = GameTimeSeconds();
        std::vector<DuelStep> keep;
        for (DuelStep const& step : _duelQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            auto event = std::find_if(_activeEvents.begin(), _activeEvents.end(), [&](ActiveEvent const& active)
            {
                return active.EventId == step.EventId && active.EventZone.Type == ActivityType::Duel && active.EndsAt > now;
            });
            if (event == _activeEvents.end())
                continue;
            Player* challenger = FindPlayer(step.ChallengerGuidLow);
            Player* target = FindPlayer(step.TargetGuidLow);
            if (!challenger || !target || challenger == target || !challenger->GetSession() || !target->GetSession())
                continue;
            if (!GET_PLAYERBOT_AI(challenger) || !IsConfiguredBotAccount(challenger->GetSession()->GetAccountId()))
                continue;
            if (step.TargetIsRealPlayer ? !IsRealPlayer(target) :
                (!GET_PLAYERBOT_AI(target) || !IsConfiguredBotAccount(target->GetSession()->GetAccountId())))
                continue;

            auto fail = [&](char const* reason, uint32 result = uint32(SPELL_CAST_OK))
            {
                uint32 until = now + std::max<uint32>(DuelFailureBackoff, _botDuelCooldownSeconds);
                _botDuelCooldown[step.ChallengerGuidLow] = std::max(_botDuelCooldown[step.ChallengerGuidLow], until);
                if (!step.TargetIsRealPlayer)
                    _botDuelCooldown[step.TargetGuidLow] = std::max(_botDuelCooldown[step.TargetGuidLow], until);
                LOG_WARN("module", "[PvPLife] Duel request deferred: {} -> {}; reason={} spell_result={} spell={} "
                    "area={}/{} dist={} phase={}/{} backoff={}s",
                    challenger->GetName(), target->GetName(), reason, result, _duelSpellId,
                    challenger->GetAreaId(), target->GetAreaId(), challenger->GetExactDist(target),
                    challenger->GetPhaseMask(), target->GetPhaseMask(), until - now);
            };
            if (!challenger->IsInWorld() || !target->IsInWorld() || !challenger->IsAlive() || !target->IsAlive() ||
                challenger->IsBeingTeleported() || target->IsBeingTeleported() ||
                challenger->IsInFlight() || target->IsInFlight())
                continue;
            if (challenger->GetMap() != target->GetMap() || !challenger->InSamePhase(target))
            {
                fail("Map or phase mismatch", SPELL_FAILED_BAD_TARGETS);
                continue;
            }

            bool matching = challenger->duel && target->duel &&
                challenger->duel->Opponent == target && target->duel->Opponent == challenger &&
                challenger->duel->Initiator == challenger && target->duel->Initiator == challenger &&
                challenger->GetGuidValue(PLAYER_DUEL_ARBITER) &&
                challenger->GetGuidValue(PLAYER_DUEL_ARBITER) == target->GetGuidValue(PLAYER_DUEL_ARBITER);
            bool started = matching &&
                (challenger->duel->State == DUEL_STATE_COUNTDOWN || challenger->duel->State == DUEL_STATE_IN_PROGRESS) &&
                (target->duel->State == DUEL_STATE_COUNTDOWN || target->duel->State == DUEL_STATE_IN_PROGRESS);
            DuelRequestAction action = EvaluateDuelRequest(step.RequestSent, challenger->duel || target->duel,
                matching, step.TargetIsRealPlayer, started, now, step.RequestDeadline);
            if (action == DuelRequestAction::Busy)
                continue;
            if (action == DuelRequestAction::Wait)
            {
                DuelStep waiting = step;
                waiting.ExecuteAt = now + 1;
                keep.push_back(waiting);
                continue;
            }
            if (action == DuelRequestAction::TimedOut)
            {
                fail("Duel state not created after cast");
                continue;
            }
            if (action == DuelRequestAction::AcceptBot || action == DuelRequestAction::Confirmed)
            {
                if (action == DuelRequestAction::AcceptBot)
                {
                    if (target->duel->State != DUEL_STATE_CHALLENGED || challenger->duel->State != DUEL_STATE_CHALLENGED)
                    {
                        fail("Duel accept state mismatch");
                        continue;
                    }
                    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
                    packet << target->GetGuidValue(PLAYER_DUEL_ARBITER);
                    target->GetSession()->HandleDuelAcceptedOpcode(packet);
                    if (!challenger->duel || !target->duel ||
                        challenger->duel->State != DUEL_STATE_COUNTDOWN || target->duel->State != DUEL_STATE_COUNTDOWN)
                    {
                        fail("Countdown not verified after duel acceptance");
                        continue;
                    }
                }
                if (step.TargetIsRealPlayer)
                {
                    _botChallengeCooldown[step.ChallengerGuidLow] = now + _botChallengeCooldownSeconds;
                    _playerChallengeCooldown[step.TargetGuidLow] = now + _realPlayerCooldownSeconds;
                }
                else
                {
                    _botDuelCooldown[step.ChallengerGuidLow] = now + _botDuelCooldownSeconds;
                    _botDuelCooldown[step.TargetGuidLow] = now + _botDuelCooldownSeconds;
                }
                LOG_INFO("module", "[PvPLife] Duel state confirmed: {} -> {}; state={}",
                    challenger->GetName(), target->GetName(), step.TargetIsRealPlayer ? "waiting/accepted by player" : "countdown/in-progress");
                AuditLog("DUEL", "event=" + std::to_string(step.EventId) + " challenger=" + challenger->GetName() +
                    " target=" + target->GetName() + " spell=" + std::to_string(_duelSpellId));
                continue;
            }

            if (challenger->IsInCombat() || target->IsInCombat() || challenger->IsPetInCombat() || target->IsPetInCombat())
            {
                fail("Self or pet in combat", SPELL_FAILED_TARGET_AFFECTING_COMBAT);
                continue;
            }
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(_duelSpellId);
            if (!spell || !spell->HasEffect(SPELL_EFFECT_DUEL))
            {
                fail("Spell lacks duel effect", SPELL_FAILED_SPELL_UNAVAILABLE);
                continue;
            }
            AreaTableEntry const* casterArea = sAreaTableStore.LookupEntry(challenger->GetAreaId());
            AreaTableEntry const* targetArea = sAreaTableStore.LookupEntry(target->GetAreaId());
            if ((casterArea && !(casterArea->flags & AREA_FLAG_ALLOW_DUELS)) ||
                (targetArea && !(targetArea->flags & AREA_FLAG_ALLOW_DUELS)))
            {
                fail("Duels forbidden in area", SPELL_FAILED_NO_DUELING);
                continue;
            }
            if (!target->GetSocial() || target->GetSocial()->HasIgnore(challenger->GetGUID()))
            {
                fail("Social info not ready or target ignored", SPELL_FAILED_BAD_TARGETS);
                continue;
            }
            SpellCastResult targetResult = spell->CheckTarget(challenger, target, false);
            if (targetResult != SPELL_CAST_OK)
            {
                fail("Target requirements not met", targetResult);
                continue;
            }
            float approachDistance = DuelApproachDistance(challenger->GetSpellMaxRangeForTarget(target, spell));
            if (challenger->GetExactDist(target) > approachDistance || !challenger->IsWithinLOSInMap(target))
            {
                DuelStep retry = step;
                if (retry.RetryCount++ < DuelApproachRetries)
                {
                    float x, y, z;
                    target->GetNearPoint(challenger, x, y, z, challenger->GetCombatReach(),
                        std::min(3.0f, approachDistance * 0.5f), target->GetAngle(challenger));
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(challenger))
                    {
                        PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                        positions["stay"].Set(x, y, z, challenger->GetMapId());
                    }
                    challenger->GetMotionMaster()->MovePoint(0, x, y, z);
                    retry.ExecuteAt = now + 3;
                    keep.push_back(retry);
                }
                else
                    fail("Approach or line-of-sight timeout", SPELL_FAILED_OUT_OF_RANGE);
                continue;
            }
            challenger->SetFacingToObject(target);
            if (!step.TargetIsRealPlayer)
            {
                target->SetFacingToObject(challenger);
                challenger->SetHealth(challenger->GetMaxHealth());
                target->SetHealth(target->GetMaxHealth());
                challenger->SetPower(challenger->getPowerType(), challenger->GetMaxPower(challenger->getPowerType()));
                target->SetPower(target->getPowerType(), target->GetMaxPower(target->getPowerType()));
            }
            SpellCastResult result = challenger->CastSpell(target, _duelSpellId, true);
            if (result != SPELL_CAST_OK)
            {
                fail("Duel spell cast failed", result);
                continue;
            }
            DuelStep waiting = step;
            waiting.RequestSent = true;
            waiting.RequestDeadline = now + DuelRequestTimeout;
            waiting.ExecuteAt = now + 1;
            keep.push_back(waiting);
        }
        _duelQueue.swap(keep);
    }

    std::string Manager::RandomChatLine(ActivityType type, ChatChannel channel, TeamSide speakerTeam)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT text,weight FROM pvp_life_chat WHERE enabled=1 AND activity_type={} AND channel={} AND (speaker_team={} OR speaker_team=2) ORDER BY id",
            static_cast<uint32>(type), static_cast<uint32>(channel), static_cast<uint32>(speakerTeam));
        if (!result)
            return {};

        struct Entry { std::string Text; uint32 Weight; };
        std::vector<Entry> entries;
        uint32 total = 0;
        do
        {
            Field* f = result->Fetch();
            Entry e{ f[0].Get<std::string>(), std::max<uint32>(1, f[1].Get<uint32>()) };
            total += e.Weight;
            entries.push_back(e);
        } while (result->NextRow());

        uint32 roll = urand(1, total);
        uint32 cursor = 0;
        for (Entry const& e : entries)
        {
            cursor += e.Weight;
            if (roll <= cursor)
                return e.Text;
        }
        return entries.empty() ? std::string() : entries.front().Text;
    }

    void Manager::ScheduleEventChat(ActiveEvent const& event)
    {
        if (event.EventZone.Type != ActivityType::ForTheHorde && event.EventZone.Type != ActivityType::ForTheAlliance)
            return;

        std::vector<uint32> defenders;
        for (Participant const& p : event.Members)
            if (!p.Attacker)
                defenders.push_back(p.Bot.GuidLow);
        if (defenders.empty())
            return;

        TeamSide team = event.EventZone.DefenderTeam;
        uint32 base = _arrivalStaggerMax + 8;
        if (urand(1, 100) <= _botChatYellChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::Yell, team);
            if (!line.empty())
                _chatQueue.push_back({ event.EventId, defenders[urand(0, static_cast<uint32>(defenders.size() - 1))],
                    GameTimeSeconds() + base + urand(1, 8), ChatChannel::Yell, line });
        }
        if (urand(1, 100) <= _botChatWorldChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::World, team);
            if (!line.empty())
                _chatQueue.push_back({ event.EventId, defenders[urand(0, static_cast<uint32>(defenders.size() - 1))],
                    GameTimeSeconds() + base + urand(8, 25), ChatChannel::World, line });
        }
    }

    void Manager::ProcessChatQueue()
    {
        uint32 now = GameTimeSeconds();
        std::vector<ChatStep> keep;
        for (ChatStep const& step : _chatQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            Player* speaker = FindPlayer(step.SpeakerGuidLow);
            if (!speaker)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(speaker);
            if (!ai)
                continue;
            if (step.Channel == ChatChannel::Yell)
                ai->Yell(step.Text);
            else
                ai->SayToWorld(step.Text);
        }
        _chatQueue.swap(keep);
    }

    void Manager::MaintainBotDuels()
    {
        if (!_duelEnable)
            return;

        uint32 now = GameTimeSeconds();
        std::unordered_set<uint32> queued;
        for (DuelStep const& step : _duelQueue)
        {
            queued.insert(step.ChallengerGuidLow);
            queued.insert(step.TargetGuidLow);
        }

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.EventZone.Type != ActivityType::Duel)
                continue;

            float centerX = (event.EventZone.RallyX + event.EventZone.TargetX) * 0.5f;
            float centerY = (event.EventZone.RallyY + event.EventZone.TargetY) * 0.5f;
            float centerZ = (event.EventZone.RallyZ + event.EventZone.TargetZ) * 0.5f;
            std::vector<uint32> available;
            for (Participant const& participant : event.Members)
            {
                uint32 guidLow = participant.Bot.GuidLow;
                if (queued.count(guidLow))
                    continue;

                Player* bot = FindPlayer(guidLow);
                if (!bot || !bot->IsAlive() || bot->IsBeingTeleported() || bot->duel || bot->IsInCombat())
                    continue;

                ApplyPvpStrategies(bot, true);
                if (bot->GetMapId() != event.EventZone.MapId ||
                    bot->GetDistance2d(centerX, centerY) > _duelLeashRadius)
                {
                    MoveBot(bot, event.EventZone.MapId, centerX + Jitter(), centerY + Jitter(), centerZ,
                        event.EventZone.RallyO, true);
                    continue;
                }

                auto duelCd = _botDuelCooldown.find(guidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                auto playerCd = _botChallengeCooldown.find(guidLow);
                if (playerCd != _botChallengeCooldown.end() && playerCd->second > now)
                    continue;

                available.push_back(guidLow);
            }

            std::unordered_set<uint32> busy;
            for (Participant const& participant : event.Members)
                if (Player* member = FindPlayer(participant.Bot.GuidLow); member && (member->duel || queued.count(participant.Bot.GuidLow)))
                    busy.insert(participant.Bot.GuidLow);
            uint32 scheduled = (static_cast<uint32>(busy.size()) + 1) / 2;
            while (available.size() >= 2 && scheduled < _duelPairLimit)
            {
                uint32 firstIndex = urand(0, static_cast<uint32>(available.size() - 1));
                uint32 first = available[firstIndex];
                Player* a = FindPlayer(first);
                if (!a)
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                std::vector<uint32> compatibleIndexes;
                for (uint32 i = 0; i < available.size(); ++i)
                {
                    if (i == firstIndex)
                        continue;
                    Player* candidate = FindPlayer(available[i]);
                    if (!candidate || a->GetTeamId() != candidate->GetTeamId())
                        continue;
                    uint32 levelDiff = std::abs(static_cast<int32>(a->GetLevel()) - static_cast<int32>(candidate->GetLevel()));
                    if (levelDiff <= _botDuelMaxLevelDifference)
                        compatibleIndexes.push_back(i);
                }

                if (compatibleIndexes.empty())
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                uint32 secondIndex = compatibleIndexes[urand(0, static_cast<uint32>(compatibleIndexes.size() - 1))];
                uint32 second = available[secondIndex];
                Player* b = FindPlayer(second);

                if (secondIndex > firstIndex)
                {
                    available.erase(available.begin() + secondIndex);
                    available.erase(available.begin() + firstIndex);
                }
                else
                {
                    available.erase(available.begin() + firstIndex);
                    available.erase(available.begin() + secondIndex);
                }

                if (b)
                {
                    ScheduleDuel(event.EventId, first, second, a->GetName(), b->GetName(), urand(_duelPairDelayMin, _duelPairDelayMax));
                    queued.insert(first);
                    queued.insert(second);
                    ++scheduled;
                }
            }
        }
    }

    void Manager::MaintainDuelParticipants()
    {
        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.EventZone.Type != ActivityType::Duel)
                continue;

            for (Participant const& participant : event.Members)
            {
                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (!bot || !bot->IsAlive() || bot->IsBeingTeleported())
                    continue;

                if (bot->duel || bot->IsInCombat())
                    continue;

                if (std::any_of(_duelQueue.begin(), _duelQueue.end(), [&](DuelStep const& step)
                    { return step.EventId == event.EventId && step.ChallengerGuidLow == participant.Bot.GuidLow; }))
                    continue;

                if (bot->GetMapId() != event.EventZone.MapId ||
                    bot->GetDistance2d(participant.ActivityX, participant.ActivityY) > _duelLeashRadius)
                {
                    MoveBot(bot, event.EventZone.MapId, participant.ActivityX, participant.ActivityY,
                        participant.ActivityZ, participant.ActivityO, true);
                    continue;
                }

                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    if (!ai->HasStrategy("duel", BOT_STATE_NON_COMBAT) ||
                        !ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                        ApplyPvpStrategies(bot, true);

                    PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                    PositionInfo stay = positions["stay"];
                    stay.Set(participant.ActivityX, participant.ActivityY, participant.ActivityZ,
                        event.EventZone.MapId);
                    positions["stay"] = stay;
                }

                if (bot->isMoving())
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    bot->ClearUnitState(UNIT_STATE_CHASE);
                    bot->ClearUnitState(UNIT_STATE_FOLLOW);
                }
            }
        }
    }

    void Manager::ProcessRealPlayerChallenges()
    {
        if (!_duelEnable || !_challengeRealPlayers)
            return;

        uint32 now = GameTimeSeconds();

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.EventZone.Type != ActivityType::Duel || !event.EventZone.ChallengePlayers)
                continue;

            for (Participant const& participant : event.Members)
            {
                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (!bot || bot->duel || bot->IsInCombat() || bot->IsBeingTeleported())
                    continue;
                auto botCd = _botChallengeCooldown.find(participant.Bot.GuidLow);
                if (botCd != _botChallengeCooldown.end() && botCd->second > now)
                    continue;
                auto duelCd = _botDuelCooldown.find(participant.Bot.GuidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                if (urand(1, 100) > _realPlayerChallengeChance)
                    continue;

                std::vector<uint32> targets;
                {
                    std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
                    HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                    for (auto const& pair : players)
                    {
                        Player* player = pair.second;
                        if (!IsRealPlayer(player) || !player->IsAlive() || player->IsGameMaster() || player->duel || player->IsInCombat())
                            continue;
                        if (player->GetSession() && player->GetSession()->GetSecurity() > SEC_PLAYER)
                            continue;
                        if (player->GetMapId() != bot->GetMapId() || player->GetTeamId() != bot->GetTeamId())
                            continue;
                        if (bot->GetDistance(player) > static_cast<float>(_realPlayerScanRadius))
                            continue;
                        uint32 diff = std::abs(static_cast<int32>(bot->GetLevel()) - static_cast<int32>(player->GetLevel()));
                        if (diff > _realPlayerMaxLevelDifference)
                            continue;
                        uint32 playerGuid = player->GetGUID().GetCounter();
                        if (!PlayerbotsLife::CanChallengeRealPlayer(playerGuid, now, _realPlayerCooldownSeconds))
                            continue;
                        auto playerCd = _playerChallengeCooldown.find(playerGuid);
                        if (playerCd != _playerChallengeCooldown.end() && playerCd->second > now)
                            continue;
                        targets.push_back(playerGuid);
                    }
                }

                if (targets.empty())
                    continue;
                uint32 targetGuid = targets[urand(0, static_cast<uint32>(targets.size() - 1))];
                Player* targetPlayer = FindPlayer(targetGuid);
                if (targetPlayer)
                {
                    ScheduleDuel(event.EventId, bot->GetGUID().GetCounter(), targetGuid,
                        bot->GetName(), targetPlayer->GetName(), urand(1, 4), true);
                    _botChallengeCooldown[bot->GetGUID().GetCounter()] = now + _botChallengeCooldownSeconds;
                    _playerChallengeCooldown[targetGuid] = now + _realPlayerCooldownSeconds;
                    PlayerbotsLife::RecordRealPlayerChallenge(targetGuid, now, _realPlayerCooldownSeconds);
                }
            }
        }
    }

    void Manager::MaintainPersistentWorldPvp()
    {
        if (!_alwaysActiveWorldPvp)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventZone.Type == ActivityType::Skirmish)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _minActiveSkirmishes)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());

        while (!zones.empty() && active < _minActiveSkirmishes && active < _maxActiveSkirmishes)
        {
            if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
                break;

            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                ++active;
                activeIds.insert(zone.Id);
            }
        }
    }

    void Manager::MaintainPersistentDuelZones()
    {
        if (!_duelEnable || !_duelAlwaysActive)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Type == ActivityType::Duel)
                activeIds.insert(e.EventZone.Id);

        std::vector<Zone> zones = LoadZones(false, static_cast<int>(ActivityType::Duel));
        for (Zone const& z : zones)
        {
            if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
                break;
            if (activeIds.find(z.Id) == activeIds.end())
                StartZone(z);
        }
    }

    void Manager::TryStartRandomSkirmish()
    {
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventZone.Type == ActivityType::Skirmish)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _maxActiveSkirmishes || urand(1, 100) > _randomSkirmishChance)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());
        if (zones.empty())
            return;

        uint32 total = 0;
        for (Zone const& z : zones) total += z.Weight;
        uint32 roll = urand(1, std::max<uint32>(1, total));
        uint32 cursor = 0;
        for (Zone const& z : zones)
        {
            cursor += z.Weight;
            if (roll <= cursor)
            {
                StartZone(z);
                return;
            }
        }
    }

    void Manager::TryStartFactionCampaign()
    {
        if ((!_forTheHordeEnable && !_forTheAllianceEnable) || urand(1, 100) > _factionCampaignChance)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 now = GameTimeSeconds();
        if (_lastFactionCampaign && _lastFactionCampaign + _factionCampaignGlobalCooldown > now)
            return;

        std::vector<Zone> zones;
        if (_forTheHordeEnable)
        {
            std::vector<Zone> fth = LoadZones(true, static_cast<int>(ActivityType::ForTheHorde));
            zones.insert(zones.end(), fth.begin(), fth.end());
        }
        if (_forTheAllianceEnable)
        {
            std::vector<Zone> fta = LoadZones(true, static_cast<int>(ActivityType::ForTheAlliance));
            zones.insert(zones.end(), fta.begin(), fta.end());
        }
        if (zones.empty())
            return;

        while (!zones.empty())
        {
            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                _lastFactionCampaign = now;
                return;
            }
        }
    }

    // Reactive Raid Response
    void Manager::TryQueueReactiveRaidResponse()
    {
        if (!_reactiveEnable || !_pendingResponses.empty())
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 now = GameTimeSeconds();
        if (_lastReactiveResponse && _lastReactiveResponse + _reactiveCooldownSeconds > now)
            return;

        std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& pair : players)
        {
            Player* leader = pair.second;
            if (!leader || !leader->GetSession() || !leader->IsInWorld() || !leader->GetMap() || leader->GetMap()->Instanceable())
                continue;
            if (!IsRealPlayer(leader) || !leader->IsPvP())
                continue;
            Group* group = leader->GetGroup();
            if (!group || !group->IsLeader(leader->GetGUID()) || group->GetMembersCount() < _reactiveMinGroupSize)
                continue;

            TeamSide responseTeam = leader->GetTeamId() == TEAM_ALLIANCE ? TeamSide::Horde : TeamSide::Alliance;
            if (responseTeam == TeamSide::Horde && !_reactiveHordeEnable)
                continue;
            if (responseTeam == TeamSide::Alliance && !_reactiveAllianceEnable)
                continue;

            uint32 desired = std::min<uint32>(group->GetMembersCount(), _reactiveMaxResponders);
            uint32 budget = CalculatePopulationBudget();
            if (desired > budget)
                desired = std::max<uint32>(2, budget);

            std::unordered_set<uint32> excluded;
            for (ActiveEvent const& event : _activeEvents)
                for (Participant const& member : event.Members)
                    excluded.insert(member.Bot.GuidLow);

            uint8 minLvl = leader->GetLevel();
            uint8 maxLvl = 80;
            if (_reactiveLevelMode == ResponderLevelMode::Fixed)
            {
                minLvl = _reactiveFixedLevel;
                maxLvl = _reactiveFixedLevel;
            }
            else
            {
                minLvl = static_cast<uint8>(std::max<int32>(1, static_cast<int32>(leader->GetLevel()) - _reactiveMaxLevelDifference));
                maxLvl = static_cast<uint8>(std::min<int32>(80, static_cast<int32>(leader->GetLevel()) + _reactiveMaxLevelDifference));
            }

            std::vector<BotCandidate> responders = LoadCandidates(responseTeam, minLvl, maxLvl, excluded);
            if (responders.size() > desired)
                responders.resize(desired);
            for (BotCandidate const& bot : responders)
                excluded.insert(bot.GuidLow);

            std::vector<uint32> pendingLogins;
            if (responders.size() < desired && _reactiveLoginOfflineBots)
            {
                std::vector<BotCandidate> offline = LoadOfflineCandidates(responseTeam, minLvl, excluded);
                uint32 need = desired - static_cast<uint32>(responders.size());
                need = std::min<uint32>(need, _reactiveMaxConcurrentLogins);
                if (offline.size() > need)
                    offline.resize(need);
                for (BotCandidate const& bot : offline)
                {
                    responders.push_back(bot);
                    pendingLogins.push_back(bot.GuidLow);
                    sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot.GuidLow), 0);
                }
            }

            if (responders.empty())
                return;

            // Reserve provisional responders
            for (BotCandidate& b : responders)
                PlayerbotsLife::TryAcquireLease(b.Token, b.GuidLow, PlayerbotsLife::ReservationOwner::PvPLife, 0, PlayerbotsLife::LeaseState::Provisional);

            PendingRaidResponse pending;
            pending.ResponseId = _nextResponseId++;
            pending.TriggerGuidLow = leader->GetGUID().GetCounter();
            pending.ResponseTeam = responseTeam;
            pending.MapId = leader->GetMapId();
            pending.TargetX = leader->GetPositionX();
            pending.TargetY = leader->GetPositionY();
            pending.TargetZ = leader->GetPositionZ();
            pending.TargetO = leader->GetOrientation();
            pending.StartAt = now + _reactiveLoginDelaySeconds;
            pending.Deadline = now + _reactiveLoginTimeoutSeconds;
            pending.Bots = std::move(responders);
            pending.PendingLoginGuids = std::move(pendingLogins);

            _pendingResponses.push_back(std::move(pending));
            _lastReactiveResponse = now;

            LOG_INFO("module", "[PvPLife] Queued {} raid response for {} invaders (leader {}).",
                TeamName(responseTeam), group->GetMembersCount(), leader->GetName());
            AuditLog("REACTIVE_START", "responseId=" + std::to_string(_pendingResponses.back().ResponseId) +
                " trigger=" + std::to_string(leader->GetGUID().GetCounter()) +
                " team=" + TeamName(responseTeam) + " responders=" + std::to_string(_pendingResponses.back().Bots.size()));
            return;
        }
    }

    void Manager::CancelPendingRaidResponse(uint64 responseId, std::string const& reason)
    {
        for (auto it = _pendingResponses.begin(); it != _pendingResponses.end(); ++it)
        {
            if (it->ResponseId == responseId)
            {
                LOG_INFO("module", "[PvPLife] Cancelled pending raid response #{} (reason: {})", responseId, reason);
                for (BotCandidate const& b : it->Bots)
                {
                    if (b.Token.GuidLow != 0)
                        PlayerbotsLife::ReleaseLease(const_cast<PlayerbotsLife::LeaseToken&>(b.Token));
                    else
                        ReleaseLifeBot(b.GuidLow);
                }
                _pendingResponses.erase(it);
                break;
            }
        }
    }

    void Manager::StartPendingRaidResponses()
    {
        uint32 now = GameTimeSeconds();
        std::vector<PendingRaidResponse> keep;

        for (PendingRaidResponse& pending : _pendingResponses)
        {
            if (pending.Cancelled)
            {
                for (BotCandidate const& b : pending.Bots)
                {
                    if (b.Token.GuidLow != 0)
                        PlayerbotsLife::ReleaseLease(const_cast<PlayerbotsLife::LeaseToken&>(b.Token));
                    else
                        ReleaseLifeBot(b.GuidLow);
                }
                continue;
            }

            if (pending.StartAt > now)
            {
                keep.push_back(pending);
                continue;
            }

            Player* leader = FindPlayer(pending.TriggerGuidLow);
            if (!leader || !leader->IsInWorld() || leader->GetMapId() != pending.MapId || !leader->IsAlive())
            {
                CancelPendingRaidResponse(pending.ResponseId, "leader vanished or map changed");
                continue;
            }

            ActiveEvent event;
            event.EventId = _nextEventId++;
            event.Origin = EventOrigin::ReactiveRaidResponse;
            event.TriggerGuidLow = pending.TriggerGuidLow;
            event.EventZone.Name = "Reactive Defense (" + std::string(TeamName(pending.ResponseTeam)) + ")";
            event.EventZone.MapId = pending.MapId;
            event.EventZone.Type = ActivityType::Skirmish;
            event.EventZone.AttackerTeam = pending.ResponseTeam;
            event.EventZone.DefenderTeam = (pending.ResponseTeam == TeamSide::Alliance) ? TeamSide::Horde : TeamSide::Alliance;
            event.StartedAt = now;
            event.EndsAt = now + _reactiveDurationMinutes * MINUTE;

            float angle = pending.TargetO + 3.14159265f;
            uint32 index = 0;
            for (BotCandidate bot : pending.Bots)
            {
                Player* player = FindPlayer(bot.GuidLow);
                if (!IsSafeBot(player, true))
                {
                    if (bot.Token.GuidLow != 0)
                        PlayerbotsLife::ReleaseLease(bot.Token);
                    else
                        ReleaseLifeBot(bot.GuidLow);
                    continue;
                }

                bot.Online = true;
                Participant part;
                part.Bot = bot;
                part.Attacker = true;
                part.OriginalMap = player->GetMapId();
                part.OriginalX = player->GetPositionX();
                part.OriginalY = player->GetPositionY();
                part.OriginalZ = player->GetPositionZ();
                part.OriginalO = player->GetOrientation();
                part.Token = bot.Token;
                PlayerbotsLife::PromoteLease(part.Token, event.EventId);
                event.Members.push_back(part);

                ApplyPvpStrategies(player, false);

                float lane = static_cast<float>(static_cast<int32>(index % _reactiveFormationColumns) -
                    static_cast<int32>(_reactiveFormationColumns / 2)) * _reactiveLaneSpacing;
                float rallyX = pending.TargetX + std::cos(angle) * _reactiveRallyDistance - std::sin(angle) * lane;
                float rallyY = pending.TargetY + std::sin(angle) * _reactiveRallyDistance + std::cos(angle) * lane;
                MoveBot(player, pending.MapId, rallyX, rallyY, pending.TargetZ, angle, _teleportOnArrival);

                float targetAdvanceX = pending.TargetX + std::cos(angle) * _reactiveAdvanceDistance + Jitter();
                float targetAdvanceY = pending.TargetY + std::sin(angle) * _reactiveAdvanceDistance + Jitter();

                ScheduleMove(event.EventId, bot.GuidLow, bot.Name, pending.MapId,
                    targetAdvanceX, targetAdvanceY, pending.TargetZ, pending.TargetO,
                    _moveAfterArrivalMin, _hardTeleportToTarget);

                if (_forceAttackOnEngage)
                    ScheduleEngagement(event.EventId, bot.GuidLow, pending.TriggerGuidLow,
                        _moveAfterArrivalMin + _engageAfterMoveSeconds);

                ++index;
            }

            if (!event.Members.empty())
            {
                LOG_INFO("module", "[PvPLife] REACTIVE EVENT #{} START responders={}",
                    event.EventId, event.Members.size());
                AuditLog("REACTIVE_START", "event=" + std::to_string(event.EventId) +
                    " trigger=" + std::to_string(pending.TriggerGuidLow) +
                    " team=" + TeamName(pending.ResponseTeam) + " responders=" + std::to_string(event.Members.size()));
                _activeEvents.push_back(event);
            }
        }
        _pendingResponses.swap(keep);
    }

    void Manager::Announce(std::string const& text) const
    {
        if (!_announce)
            return;
        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text);
    }

    void Manager::Update(uint32 diff)
    {
        if (!_enable)
            return;

        if (_startupElapsedMs < _startupDelaySeconds * IN_MILLISECONDS)
        {
            _startupElapsedMs += diff;
            if (!_startupDelayLogged)
            {
                _startupDelayLogged = true;
                LOG_INFO("module", "[PvPLife] Startup delay active for {} seconds so Playerbots can finish logging in.",
                    _startupDelaySeconds);
            }
            return;
        }

        ProcessMoveQueue();
        ProcessDuelQueue();
        ProcessEngagementQueue();
        ProcessChatQueue();
        ExpireEvents();
        StartPendingRaidResponses();

        if (_duelGuardTimerMs <= diff)
        {
            _duelGuardTimerMs = _duelGuardIntervalMs;
            MaintainDuelParticipants();
        }
        else
            _duelGuardTimerMs -= diff;

        if (_timerMs <= diff)
        {
            _timerMs = _tickSeconds * IN_MILLISECONDS;
            TryQueueReactiveRaidResponse();
            MaintainPersistentDuelZones();
            MaintainPersistentWorldPvp();
            TryStartRandomSkirmish();
            TryStartFactionCampaign();
            MaintainBotDuels();
            ProcessRealPlayerChallenges();
        }
        else
            _timerMs -= diff;
    }

    void Manager::PrintStatus(ChatHandler* handler) const
    {
        uint32 startupTarget = _startupDelaySeconds * IN_MILLISECONDS;
        uint32 startupLeft = _startupElapsedMs < startupTarget ?
            (startupTarget - _startupElapsedMs + 999) / 1000 : 0;
        handler->PSendSysMessage("PvP Life: enabled={} DB={} startup delay={}s events={} pendingResponses={} move={} duel={} engage={}",
            _enable ? 1 : 0, _databaseReady ? "ready" : "missing", startupLeft,
            _activeEvents.size(), _pendingResponses.size(), _moveQueue.size(), _duelQueue.size(), _engagementQueue.size());

        uint32 budget = CalculatePopulationBudget();
        handler->PSendSysMessage(
            "Population: budget={} mode={} borrowPercent={}% maxTotal={} bots: prefix='{}' respectActivity={} minOnlineReq={}",
            budget, _budgetMode == BudgetMode::Percentage ? "percentage" : "fixed", _borrowPercent, _maxTotal,
            _botAccountPrefix, _respectPlayerbotActivity ? 1 : 0, _minOnlineRequired);

        uint32 now = GameTimeSeconds();
        for (ActiveEvent const& e : _activeEvents)
        {
            uint32 left = e.EndsAt > now ? e.EndsAt - now : 0;
            handler->PSendSysMessage("#{} {} [{}] origin={} level {}~{} bots={} time left={}m",
                e.EventId, e.EventZone.Name, TypeName(e.EventZone.Type),
                e.Origin == EventOrigin::ReactiveRaidResponse ? "ReactiveDefense" : "Zone",
                e.EventZone.MinLevel, e.EventZone.MaxLevel,
                e.Members.size(), left / MINUTE);
        }

        for (PendingRaidResponse const& p : _pendingResponses)
        {
            uint32 left = p.StartAt > now ? p.StartAt - now : 0;
            handler->PSendSysMessage("[Pending] Reactive Defense team={} triggerGuid={} responders={} startsIn={}s",
                TeamName(p.ResponseTeam), p.TriggerGuidLow, p.Bots.size(), left);
        }
    }

    void Manager::PrintScan(ChatHandler* handler, uint8 minLevel, uint8 maxLevel) const
    {
        uint8 effectiveMin = EffectiveMinLevel(minLevel);
        uint8 effectiveMax = maxLevel;
        if (effectiveMax < effectiveMin)
            effectiveMax = effectiveMin;

        uint32 alliance = 0;
        uint32 horde = 0;
        uint32 total = 0;

        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=1 AND level BETWEEN {} AND {} ORDER BY level DESC LIMIT {}",
            effectiveMin, effectiveMax, _botQueryLimit);

        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                BotCandidate b;
                b.GuidLow = f[0].Get<uint32>();
                b.AccountId = f[1].Get<uint32>();
                b.Name = f[2].Get<std::string>();
                b.Level = f[3].Get<uint8>();
                b.Race = f[4].Get<uint8>();
                b.Class = f[5].Get<uint8>();
                b.Online = f[6].Get<uint8>() != 0;
                b.Team = TeamForRace(b.Race);

                if (!const_cast<Manager*>(this)->IsConfiguredBotAccount(b.AccountId))
                    continue;
                if (!IsSafeBot(FindPlayer(b.GuidLow)))
                    continue;

                ++total;
                if (b.Team == TeamSide::Alliance)
                    ++alliance;
                else if (b.Team == TeamSide::Horde)
                    ++horde;
            } while (result->NextRow());
        }

        handler->PSendSysMessage("PvP Life scan lvl {}..{} prefix='{}': Alliance={} Horde={} Total={} queryLimit={} globalMin={}.",
            effectiveMin, effectiveMax, _botAccountPrefix, alliance, horde, total, _botQueryLimit, _globalMinLevel);

        std::vector<Zone> spots = const_cast<Manager*>(this)->LoadZones(true);
        if (spots.empty())
        {
            handler->PSendSysMessage("No enabled/cooldown-ready hotspots right now.");
            return;
        }

        std::unordered_set<uint32> exclude;
        uint32 shown = 0;
        for (Zone const& h : spots)
        {
            if (shown >= 12)
                break;
            std::vector<BotCandidate> attackers = const_cast<Manager*>(this)->LoadCandidates(h.AttackerTeam, h.MinLevel, h.MaxLevel, exclude);
            std::vector<BotCandidate> defenders = const_cast<Manager*>(this)->LoadCandidates(h.DefenderTeam, h.MinLevel, h.MaxLevel, exclude);
            handler->PSendSysMessage("{}: {} attackers={} need={}..{} / {} defenders={} need={}..{} lvl={}..{} weight={}",
                h.Name, TeamName(h.AttackerTeam), attackers.size(), h.AttackersMin, h.AttackersMax,
                TeamName(h.DefenderTeam), defenders.size(), h.DefendersMin, h.DefendersMax,
                EffectiveMinLevel(h.MinLevel), h.MaxLevel, h.Weight);
            ++shown;
        }
    }

    void Manager::PrintCompat(ChatHandler* handler) const
    {
        uint32 activeFlags = PlayerbotsLife::GetActiveModules();
        bool clActive = (activeFlags & PlayerbotsLife::MODULE_CITY_LIFE) != 0;
        bool pvpActive = (activeFlags & PlayerbotsLife::MODULE_PVP_LIFE) != 0;
        bool artActive = (activeFlags & PlayerbotsLife::MODULE_ARTISANS) != 0;

        handler->PSendSysMessage("=== Life Modules Compatibility & Coexistence ===");
        handler->PSendSysMessage("CityLife: {}", clActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");
        handler->PSendSysMessage("PvPLife:  {}", pvpActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");
        handler->PSendSysMessage("Artisans: {}", artActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");

        handler->PSendSysMessage("Anti-Stealing Harmony: {}", _harmonyEnable ? "ENABLED" : "DISABLED");
        handler->PSendSysMessage("Respect Reservations:  {}", _respectCityLifeReservations ? "ENABLED" : "DISABLED");
        handler->PSendSysMessage("Exclude CL Accounts:   {}", _excludeCityLifeAccounts ? "ENABLED" : "DISABLED");
        handler->PSendSysMessage("Hooks Pass-Through:    {}", _independentHookProcessing ? "ENABLED" : "DISABLED");

        PrintReservations(handler);
    }

    void Manager::PrintReservations(ChatHandler* handler) const
    {
        PlayerbotsLife::ReservationRegistry& reg = PlayerbotsLife::GetReservationRegistry();
        std::lock_guard<std::mutex> lock(reg.Mutex);

        uint32 clCount = 0;
        uint32 pvpCount = 0;
        uint32 artCount = 0;

        for (auto const& pair : reg.Owners)
        {
            if (pair.second == PlayerbotsLife::ReservationOwner::CityLife)
                ++clCount;
            else if (pair.second == PlayerbotsLife::ReservationOwner::PvPLife)
                ++pvpCount;
            else if (pair.second == PlayerbotsLife::ReservationOwner::Artisans)
                ++artCount;
        }

        handler->PSendSysMessage("Total Reserved Bots: {} (CityLife: {}, PvPLife: {}, Artisans: {})",
            reg.Owners.size(), clCount, pvpCount, artCount);
    }

    void Manager::PrintDebugStatus(ChatHandler* handler) const
    {
        handler->PSendSysMessage("=== PvPLife Debug Counters ===");
        handler->PSendSysMessage("Active Events: {}", _activeEvents.size());
        handler->PSendSysMessage("Pending Moves: {}", _moveQueue.size());
        handler->PSendSysMessage("Pending Duels: {}", _duelQueue.size());
        handler->PSendSysMessage("Pending Engagements: {}", _engagementQueue.size());
        handler->PSendSysMessage("Pending Chats: {}", _chatQueue.size());
        handler->PSendSysMessage("Pending Raid Responses: {}", _pendingResponses.size());
        handler->PSendSysMessage("Cached Bot Accounts: {}", _botAccountCache.size());
    }

    void Manager::PrintHelp(ChatHandler* handler, bool isWpvp) const
    {
        std::string cmdPrefix = isWpvp ? ".wpvp" : ".pvplife";
        handler->PSendSysMessage("{} commands:", cmdPrefix);
        handler->PSendSysMessage("{} status", cmdPrefix);
        handler->PSendSysMessage("{} scan [minLvl] [maxLvl]", cmdPrefix);
        handler->PSendSysMessage("{} start <zone|spot>", cmdPrefix);
        handler->PSendSysMessage("{} stop <id|all>", cmdPrefix);
        handler->PSendSysMessage("{} compat", cmdPrefix);
        handler->PSendSysMessage("{} reservations", cmdPrefix);
        handler->PSendSysMessage("{} response status | stop <id|all>", cmdPrefix);
        handler->PSendSysMessage("{} zone list", cmdPrefix);
        handler->PSendSysMessage("{} zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>", cmdPrefix);
        handler->PSendSysMessage("{} zone rally <name> | target <name>   (save GM position)", cmdPrefix);
        handler->PSendSysMessage("{} zone counts <name> <attMin> <attMax> <defMin> <defMax>", cmdPrefix);
        handler->PSendSysMessage("{} zone duration <name> <minM> <maxM>", cmdPrefix);
        handler->PSendSysMessage("{} zone weight <name> <weight> | cooldown <name> <seconds>", cmdPrefix);
        handler->PSendSysMessage("{} zone challenge <name> <0|1> | chat <name> <0|1> | enable <name> <0|1>", cmdPrefix);
        if (isWpvp)
        {
            handler->PSendSysMessage(".wpvp spot list | create | rally | target | counts | duration | enable");
        }
    }

    bool Manager::HandleZoneCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintHelp(handler);
            return true;
        }
        std::string action = Lower(args[1]);
        if (action == "list")
        {
            QueryResult result = WorldDatabase.Query(ZoneSelectSql("ORDER BY activity_type,name").c_str());
            if (!result)
            {
                handler->PSendSysMessage("PvP Life: No configured zones found.");
                return true;
            }
            do
            {
                Zone z = ReadZone(result->Fetch());
                ApplyZoneConfig(z);
                handler->PSendSysMessage("#{} {} [{}] enabled={} {}->{} level={}~{} pop={}~{} ratio={}~{} / {}~{} cooldown={}s challenge={} chat={}",
                    z.Id, z.Name, TypeName(z.Type), z.Enabled ? 1 : 0, TeamName(z.AttackerTeam), TeamName(z.DefenderTeam),
                    z.MinLevel, z.MaxLevel, z.PopulationMin, z.PopulationMax,
                    z.AttackersMin, z.AttackersMax, z.DefendersMin, z.DefendersMax,
                    z.CooldownSeconds, z.ChallengePlayers ? 1 : 0, z.BotChat ? 1 : 0);
            } while (result->NextRow());
            return true;
        }
        if (action == "create")
        {
            if (args.size() < 8)
            {
                handler->PSendSysMessage("Syntax: .pvplife zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command requires an in-game GM position.");
                return true;
            }
            ActivityType type;
            TeamSide attacker, defender;
            if (!ParseType(args[3], type) || !ParseTeam(args[4], attacker) || !ParseTeam(args[5], defender))
            {
                handler->PSendSysMessage("Invalid activity type or faction value.");
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            uint32 minLevel = std::clamp<uint32>(std::stoul(args[6]), 1, 80);
            uint32 maxLevel = std::clamp<uint32>(std::stoul(args[7]), minLevel, 80);
            WorldDatabase.Execute(
                "INSERT INTO pvp_life_zone (name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
                "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o) VALUES ('{}',1,{},{},{},{},{},{},{},{},{},{},{},{},{},{}) "
                "ON DUPLICATE KEY UPDATE activity_type=VALUES(activity_type),attacker_team=VALUES(attacker_team),defender_team=VALUES(defender_team),"
                "min_level=VALUES(min_level),max_level=VALUES(max_level)",
                name, static_cast<uint32>(type), static_cast<uint32>(attacker), static_cast<uint32>(defender), minLevel, maxLevel,
                gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(),
                gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation());
            handler->PSendSysMessage("PvP Life: Created/updated {}. Now set rally and target positions.", args[2]);
            return true;
        }
        if ((action == "rally" || action == "target") && args.size() >= 3)
        {
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command requires an in-game GM position.");
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            if (action == "rally")
            {
                WorldDatabase.Execute("UPDATE pvp_life_zone SET map_id={},rally_x={},rally_y={},rally_z={},rally_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name);
            }
            else
            {
                WorldDatabase.Execute("UPDATE pvp_life_zone SET map_id={},target_x={},target_y={},target_z={},target_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name);
            }
            handler->PSendSysMessage("PvP Life: Saved {} position for {}.", action, args[2]);
            return true;
        }
        if (action == "counts" && args.size() >= 7)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE pvp_life_zone SET attackers_min={},attackers_max={},defenders_min={},defenders_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), std::stoul(args[5]), std::stoul(args[6]), name);
            handler->PSendSysMessage("PvP Life: Updated counts for {}.", args[2]);
            return true;
        }
        if (action == "duration" && args.size() >= 5)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE pvp_life_zone SET duration_min={},duration_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), name);
            handler->PSendSysMessage("PvP Life: Updated duration for {}.", args[2]);
            return true;
        }
        if ((action == "weight" || action == "cooldown" || action == "challenge" || action == "chat" || action == "enable") && args.size() >= 4)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            uint32 value = std::stoul(args[3]);
            if (action == "weight")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET weight={} WHERE name='{}'", value, name);
            else if (action == "cooldown")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET cooldown_seconds={} WHERE name='{}'", value, name);
            else if (action == "challenge")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET challenge_players={} WHERE name='{}'", value ? 1 : 0, name);
            else if (action == "chat")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET bot_chat={} WHERE name='{}'", value ? 1 : 0, name);
            else
                WorldDatabase.Execute("UPDATE pvp_life_zone SET enabled={} WHERE name='{}'", value ? 1 : 0, name);
            handler->PSendSysMessage("PvP Life: Updated {} for {}.", action, args[2]);
            return true;
        }
        PrintHelp(handler);
        return true;
    }

    bool Manager::HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintHelp(handler, true);
            return true;
        }
        std::string action = Lower(args[1]);
        if (action == "list")
        {
            std::vector<std::string> forwardArgs = { "zone", "list" };
            return HandleZoneCommand(handler, forwardArgs);
        }
        if (action == "create")
        {
            if (args.size() < 7)
            {
                handler->PSendSysMessage("Syntax: .wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            TeamSide attacker, defender;
            if (!ParseTeam(args[3], attacker) || !ParseTeam(args[4], defender))
            {
                handler->PSendSysMessage("Bad team. Use alliance or horde.");
                return true;
            }
            ActivityType type = (attacker != TeamSide::Any && attacker == defender) ? ActivityType::Duel : ActivityType::Skirmish;
            std::string typeStr = (type == ActivityType::Duel) ? "duel" : "skirmish";
            std::vector<std::string> forwardArgs = { "zone", "create", args[2], typeStr, args[3], args[4], args[5], args[6] };
            return HandleZoneCommand(handler, forwardArgs);
        }
        if (action == "rally" || action == "target" || action == "counts" || action == "duration" || action == "enable")
        {
            std::vector<std::string> forwardArgs = args;
            forwardArgs[0] = "zone";
            return HandleZoneCommand(handler, forwardArgs);
        }
        PrintHelp(handler, true);
        return true;
    }

    bool Manager::HandleResponseCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            handler->PSendSysMessage("Syntax: .pvplife response status | stop <id|all>");
            return true;
        }
        std::string sub = Lower(args[1]);
        if (sub == "status")
        {
            handler->PSendSysMessage("=== Reactive Raid Responses ===");
            handler->PSendSysMessage("Pending: {}", _pendingResponses.size());
            uint32 now = GameTimeSeconds();
            for (PendingRaidResponse const& p : _pendingResponses)
            {
                uint32 left = p.StartAt > now ? p.StartAt - now : 0;
                handler->PSendSysMessage("#{} team={} triggerGuid={} responders={} startsIn={}s",
                    p.ResponseId, TeamName(p.ResponseTeam), p.TriggerGuidLow, p.Bots.size(), left);
            }
            return true;
        }
        if (sub == "stop" && args.size() >= 3)
        {
            if (Lower(args[2]) == "all")
            {
                for (PendingRaidResponse const& p : _pendingResponses)
                    CancelPendingRaidResponse(p.ResponseId, "gm-stop-all");
                _pendingResponses.clear();
                handler->PSendSysMessage("PvP Life: Stopped all pending raid responses.");
                return true;
            }
            uint64 id = std::stoull(args[2]);
            CancelPendingRaidResponse(id, "gm-stop");
            handler->PSendSysMessage("PvP Life: Stopped pending response #{}.", id);
            return true;
        }
        handler->PSendSysMessage("Syntax: .pvplife response status | stop <id|all>");
        return true;
    }

    bool Manager::HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintDebugStatus(handler);
            return true;
        }
        std::string sub = Lower(args[1]);
        if (sub == "status" || sub == "counters")
        {
            PrintDebugStatus(handler);
            return true;
        }
        if (sub == "validate")
        {
            bool db = VerifyDatabase();
            handler->PSendSysMessage("Database Validation: {}", db ? "OK" : "ERROR");
            PrintReservations(handler);
            return true;
        }
        if (sub == "bot" && args.size() >= 3)
        {
            uint32 guidLow = 0;
            if (std::all_of(args[2].begin(), args[2].end(), ::isdigit))
                guidLow = std::stoul(args[2]);
            else
            {
                Player* p = ObjectAccessor::FindPlayerByName(args[2]);
                if (p)
                    guidLow = p->GetGUID().GetCounter();
            }

            if (!guidLow)
            {
                handler->PSendSysMessage("Bot '{}' not found.", args[2]);
                return true;
            }

            Player* p = FindPlayer(guidLow);
            bool isPart = IsParticipant(guidLow);
            bool isRes = PlayerbotsLife::IsReserved(guidLow);
            handler->PSendSysMessage("Bot guid={}: online={} participant={} reserved={}",
                guidLow, p ? "YES" : "NO", isPart ? "YES" : "NO", isRes ? "YES" : "NO");
            return true;
        }
        PrintDebugStatus(handler);
        return true;
    }

    void Manager::StopEventById(uint64 id, ChatHandler* handler)
    {
        if (id == 0)
        {
            for (ActiveEvent const& e : _activeEvents)
                EndEvent(e, "gm-stop");
            _activeEvents.clear();
            _moveQueue.clear();
            _duelQueue.clear();
            _engagementQueue.clear();
            _chatQueue.clear();
            if (handler)
                handler->PSendSysMessage("PvP Life: Stopped all activities.");
            return;
        }

        bool found = false;
        std::vector<ActiveEvent> keep;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventId == id)
            {
                EndEvent(e, "gm-stop");
                found = true;
            }
            else
                keep.push_back(e);
        }
        _activeEvents.swap(keep);
        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(), [id](MoveStep const& s) { return s.EventId == id; }), _moveQueue.end());
        _duelQueue.erase(std::remove_if(_duelQueue.begin(), _duelQueue.end(), [id](DuelStep const& s) { return s.EventId == id; }), _duelQueue.end());
        _engagementQueue.erase(std::remove_if(_engagementQueue.begin(), _engagementQueue.end(), [id](EngagementStep const& s) { return s.EventId == id; }), _engagementQueue.end());
        _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(), [id](ChatStep const& s) { return s.EventId == id; }), _chatQueue.end());
        if (handler)
            handler->PSendSysMessage("PvP Life: {} #{}.", found ? "stopped" : "did not find", id);
    }

    bool Manager::HandleCommand(ChatHandler* handler, std::string const& text)
    {
        std::vector<std::string> args = Tokenize(text);
        if (!args.empty() && (Lower(args[0]) == ".pvplife" || Lower(args[0]) == "pvplife"))
            args.erase(args.begin());
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler);
            return true;
        }
        std::string cmd = Lower(args[0]);
        if (cmd == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (cmd == "scan")
        {
            uint8 minLevel = args.size() >= 2 ? static_cast<uint8>(std::clamp<uint32>(std::stoul(args[1]), 1, 80)) : _globalMinLevel;
            uint8 maxLevel = args.size() >= 3 ? static_cast<uint8>(std::clamp<uint32>(std::stoul(args[2]), minLevel, 80)) : 80;
            PrintScan(handler, minLevel, maxLevel);
            return true;
        }
        if (cmd == "compat")
        {
            PrintCompat(handler);
            return true;
        }
        if (cmd == "reservations")
        {
            PrintReservations(handler);
            return true;
        }
        if (cmd == "reload")
        {
            LoadConfig();
            handler->PSendSysMessage("PvP Life: Configuration and zone definitions reloaded.");
            return true;
        }
        if (cmd == "response")
            return HandleResponseCommand(handler, args);
        if (cmd == "debug")
            return HandleDebugCommand(handler, args);
        if (cmd == "zone")
            return HandleZoneCommand(handler, args);
        if (cmd == "start" && args.size() >= 2)
        {
            Zone zone;
            if (!LoadZoneByName(args[1], zone))
            {
                handler->PSendSysMessage("PvP Life: Zone '{}' not found.", args[1]);
                return true;
            }
            StartZone(zone, handler, true);
            return true;
        }
        if (cmd == "stop")
        {
            if (args.size() < 2 || Lower(args[1]) == "all")
            {
                StopEventById(0, handler);
                return true;
            }
            uint64 id = std::stoull(args[1]);
            StopEventById(id, handler);
            return true;
        }
        PrintHelp(handler);
        return true;
    }

    bool Manager::HandleWpvpCommand(ChatHandler* handler, std::string const& text)
    {
        std::vector<std::string> args = Tokenize(text);
        if (!args.empty() && (Lower(args[0]) == ".wpvp" || Lower(args[0]) == "wpvp"))
            args.erase(args.begin());
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler, true);
            return true;
        }
        std::string cmd = Lower(args[0]);
        if (cmd == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (cmd == "scan")
        {
            uint8 minLevel = args.size() >= 2 ? static_cast<uint8>(std::clamp<uint32>(std::stoul(args[1]), 1, 80)) : _globalMinLevel;
            uint8 maxLevel = args.size() >= 3 ? static_cast<uint8>(std::clamp<uint32>(std::stoul(args[2]), minLevel, 80)) : 80;
            PrintScan(handler, minLevel, maxLevel);
            return true;
        }
        if (cmd == "compat")
        {
            PrintCompat(handler);
            return true;
        }
        if (cmd == "spot")
            return HandleSpotCommand(handler, args);
        if (cmd == "start" && args.size() >= 2)
        {
            Zone zone;
            if (!LoadZoneByName(args[1], zone))
            {
                handler->PSendSysMessage("PlayerbotWorldPvp: Spot '{}' not found.", args[1]);
                return true;
            }
            StartZone(zone, handler, true);
            return true;
        }
        if (cmd == "stop")
        {
            if (args.size() < 2 || Lower(args[1]) == "all")
            {
                StopEventById(0, handler);
                return true;
            }
            uint64 id = std::stoull(args[1]);
            StopEventById(id, handler);
            return true;
        }
        PrintHelp(handler, true);
        return true;
    }
}
