/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PvPLifeMgr.h"
#include "LifeBotReservation.h"

#include "Chat.h"
#include "CommandScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

class PvPLifeWorldScript : public WorldScript
{
public:
    PvPLifeWorldScript()
        : WorldScript("PvPLifeWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPvPLifeMgr.LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sPvPLifeMgr.Update(diff);
    }

    void OnShutdown() override
    {
        sPvPLifeMgr.Shutdown();
        PlayerbotsLife::UnregisterActiveModule(PlayerbotsLife::MODULE_PVP_LIFE);
    }
};

class PvPLifePlayerScript : public PlayerScript
{
public:
    PvPLifePlayerScript()
        : PlayerScript("PvPLifePlayerScript", {
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_CAN_GROUP_INVITE,
            PLAYERHOOK_CAN_GROUP_ACCEPT
        })
    {
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            sPvPLifeMgr.DisengageParticipant(player->GetGUID().GetCounter());
    }

    bool OnPlayerCanGroupInvite(Player* /*player*/, std::string& membername) override
    {
        if (Player* invited = ObjectAccessor::FindPlayerByName(membername))
        {
            if (invited->GetSession() && invited->GetSession()->IsBot())
            {
                if (sPvPLifeMgr.IsParticipant(invited->GetGUID().GetCounter()))
                    sPvPLifeMgr.DisengageParticipant(invited->GetGUID().GetCounter());
            }
        }
        return true;
    }

    bool OnPlayerCanGroupAccept(Player* player, Group* /*group*/) override
    {
        if (player && player->GetSession() && player->GetSession()->IsBot())
        {
            if (sPvPLifeMgr.IsParticipant(player->GetGUID().GetCounter()))
                sPvPLifeMgr.DisengageParticipant(player->GetGUID().GetCounter());
        }
        return true;
    }
};

class PvPLifeCommandScript : public CommandScript
{
public:
    PvPLifeCommandScript() : CommandScript("PvPLifeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "pvplife", HandlePvPLifeCommand, SEC_GAMEMASTER,    Console::Yes },
            { "wpvp",    HandleWpvpCommand,    SEC_ADMINISTRATOR, Console::Yes }
        };
        return commandTable;
    }

    static bool HandlePvPLifeCommand(ChatHandler* handler, char const* args)
    {
        return sPvPLifeMgr.HandleCommand(handler, args ? args : "");
    }

    static bool HandleWpvpCommand(ChatHandler* handler, char const* args)
    {
        return sPvPLifeMgr.HandleWpvpCommand(handler, args ? args : "");
    }
};

void AddPvPLifeScripts()
{
    PlayerbotsLife::RegisterActiveModule(PlayerbotsLife::MODULE_PVP_LIFE);
    new PvPLifeWorldScript();
    new PvPLifePlayerScript();
    new PvPLifeCommandScript();
    LOG_INFO("server.loading", ">> Loaded mod-playerbots-pvp-life");
}
