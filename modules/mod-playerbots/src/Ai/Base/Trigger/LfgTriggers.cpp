/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgTriggers.h"

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
    {
        lastMapId = 0;
        lastInstanceId = 0;
        enteredInstanceAt = 0;
        return false;
    }

    if (bot->GetMapId() != lastMapId || bot->GetInstanceId() != lastInstanceId)
    {
        lastMapId = bot->GetMapId();
        lastInstanceId = bot->GetInstanceId();
        enteredInstanceAt = time(nullptr);
    }

    if (!enteredInstanceAt || time(nullptr) - enteredInstanceAt < sPlayerbotAIConfig.lfgLatecomerGraceSeconds)
        return false;

    return AI_VALUE(Unit*, "lfg latecomer") != nullptr;
}
