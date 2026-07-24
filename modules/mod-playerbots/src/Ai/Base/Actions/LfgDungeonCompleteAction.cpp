/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgDungeonCompleteAction.h"

#include "Config.h"
#include "Event.h"
#include "LFGMgr.h"
#include "Playerbots.h"
#include "Timer.h"

bool LfgDungeonCompleteAction::isUseful() { return bot->GetGroup() != nullptr; }

bool LfgDungeonCompleteAction::Execute(Event /*event*/)
{
    // Re-derive the trigger's conditions - Engine may run this a tick after the check, and the
    // group/map state is what the per-instance bookkeeping is keyed on anyway.
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // GROUP guid, matching LfgDungeonCompleteTrigger and the LfgTeleportAction exit-mirror guard:
    // LFGMgr::FinishDungeon() sets the group's state first and unconditionally.
    if (sLFGMgr->GetState(group->GetGUID()) != lfg::LFG_STATE_FINISHED_DUNGEON)
        return false;

    // First observation in this run (or a stale record from a previous one): start the linger
    // clock and re-arm the announcement.
    if (!completeSince || completeGroupCounter != group->GetGUID().GetCounter() ||
        completeMapId != bot->GetMapId() || completeInstanceId != bot->GetInstanceId())
    {
        completeGroupCounter = group->GetGUID().GetCounter();
        completeMapId = bot->GetMapId();
        completeInstanceId = bot->GetInstanceId();
        completeSince = getMSTime();
        announced = false;
    }

    // One line, from one member: the navigation driver if the run has one, else the leader-flag
    // holder. Every member's action instance evaluates the condition exactly ONCE per run - not
    // once per fire - because the announcer role cascades as bots trickle out (the driver leaves,
    // leadership/driver selection lands on a still-lingering bot), and re-evaluating then would
    // repeat the line the original announcer already said. All bots make their single evaluation
    // within one trigger interval of the finish, minutes before the first leave, so they agree on
    // who the announcer is. SayToParty only delivers to real players, so an all-bot group says it
    // into the void for free.
    Player* driver = botAI->GetDungeonNavigationLeader();
    if (!announced)
    {
        announced = true;
        if (driver ? driver == bot : group->IsLeader(bot->GetGUID()))
            botAI->SayToParty("Good run! That's the place cleared.");
    }

    // Only random bots take themselves out: a player's active alt and non-random (personal) bots
    // are their owner's to dismiss, and both are non-random accounts, so IsRandomBot alone draws
    // that line. Deliberately NOT HasActivePlayerMaster(): in a pug, FindNewMaster() makes the
    // real player every random bot's master, so that check would exempt exactly the mixed groups
    // this exists for. A real player still in the group does NOT keep random bots around past the
    // linger - that's the pug experience.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;

    // Kill switch gates only the leave; the announcement above still happens.
    static bool const autoLeave = sConfigMgr->GetOption<bool>("AiPlayerbot.LfgCompleteAutoLeave", true);
    if (!autoLeave)
        return true;

    // Linger to loot/rest like a player would, with a stable guid-derived 0-60s spread so the
    // group trickles out over a minute instead of vanishing simultaneously.
    static uint32 const lingerSeconds =
        sConfigMgr->GetOption<uint32>("AiPlayerbot.LfgCompleteLingerSeconds", 150);
    uint32 const jitterSeconds = bot->GetGUID().GetCounter() % 61;
    if (GetMSTimeDiffToNow(completeSince) < (lingerSeconds + jitterSeconds) * 1000)
        return true;

    LOG_INFO("playerbots", "Bot {} <{}>: LFG run finished {}s ago - leaving the group",
             bot->GetGUID().ToString(), bot->GetName(), (lingerSeconds + jitterSeconds));

    // The group leave is the whole exit sequence: core LFGScripts::OnRemoveMember teleports the
    // leaver back to its entry point (state FINISHED_DUNGEON also skips the deserter debuff), and
    // clears the member's LFG state - see the class comment in the header.
    botAI->LeaveOrDisbandGroup();
    return true;
}
