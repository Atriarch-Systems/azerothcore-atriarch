/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgLeaderAnnounceAction.h"

#include "Event.h"
#include "Playerbots.h"

#include <sstream>

bool LfgLeaderAnnounceAction::isUseful()
{
    Group* group = bot->GetGroup();
    return group && group->IsLeader(bot->GetGUID());
}

bool LfgLeaderAnnounceAction::Execute(Event /*event*/)
{
    // Re-derive the trigger's conditions - Engine may run this a tick after the check, and the
    // group/leadership/map state is what the one-shot key is built from anyway.
    Group* group = bot->GetGroup();
    if (!group || !group->IsLeader(bot->GetGUID()))
        return false;

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    if (botAI->GetRealPlayersInGroup().empty())
        return false;

    std::ostringstream key;
    key << group->GetGUID().GetCounter() << ':' << bot->GetMapId() << ':' << bot->GetInstanceId();
    if (announcedKey == key.str())
        return false;

    if (!botAI->SayToParty("I'll lead - say 'give me lead' if you'd rather lead."))
        return false;

    announcedKey = key.str();

    LOG_INFO("playerbots", "Bot {} <{}>: announced dungeon leadership to its LFG group",
             bot->GetGUID().ToString(), bot->GetName());
    return true;
}
