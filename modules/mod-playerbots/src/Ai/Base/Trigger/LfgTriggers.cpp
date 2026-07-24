/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgTriggers.h"

#include "LFGMgr.h"
#include "Playerbots.h"

bool LfgProposalActiveTrigger::IsActive() { return AI_VALUE(uint32, "lfg proposal"); }

bool UnknownDungeonTrigger::IsActive()
{
    return botAI->HasActivePlayerMaster() && botAI->GetMaster() && botAI->GetMaster()->IsInWorld() &&
           botAI->GetMaster()->GetMap()->IsDungeon() && bot->GetMapId() == botAI->GetMaster()->GetMapId();
}

bool LfgLatecomerTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || !group->IsLeader(bot->GetGUID()))
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // The grace period (per member, from when the member actually left the bot's instance) is
    // evaluated inside LfgLatecomerValue::Calculate() -- see LfgValues.h.
    return AI_VALUE(Unit*, "lfg latecomer") != nullptr;
}

bool LfgLeaderAnnounceTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group || !group->IsLeader(bot->GetGUID()))
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // GetRealPlayersInGroup() skips playerbots but counts self-bot humans as real, matching
    // LfgLatecomerValue's member classification. Members are only iterable while online, so this
    // is "a real player is present" in the useful sense - the once-per-group+instance gate is the
    // action's job (see LfgLeaderAnnounceAction.h).
    return !botAI->GetRealPlayersInGroup().empty();
}

bool LfgDungeonCompleteTrigger::IsActive()
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // GROUP guid on purpose: LFGMgr::FinishDungeon() flips the group's state the moment the final
    // encounter completes, while the per-player state only follows for members it could reward -
    // same query the LfgTeleportAction exit-mirror guard already relies on.
    return sLFGMgr->GetState(group->GetGUID()) == lfg::LFG_STATE_FINISHED_DUNGEON;
}
