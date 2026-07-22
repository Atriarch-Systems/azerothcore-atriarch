/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgValues.h"

#include "Group.h"
#include "Map.h"
#include "Playerbots.h"

void LfgLatecomerValue::ResetTracking()
{
    lastMapId = 0;
    lastInstanceId = 0;
    botEnteredAt = 0;
    lastSeenOnMap.clear();
}

Unit* LfgLatecomerValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        ResetTracking();
        return nullptr;
    }

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
    {
        ResetTracking();
        return nullptr;
    }

    time_t const now = time(nullptr);

    if (bot->GetMapId() != lastMapId || bot->GetInstanceId() != lastInstanceId)
    {
        lastMapId = bot->GetMapId();
        lastInstanceId = bot->GetInstanceId();
        botEnteredAt = now;
        lastSeenOnMap.clear();
    }

    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (slot.guid == bot->GetGUID())
            continue;

        Player* member = ObjectAccessor::FindPlayer(slot.guid);
        if (!member || !member->IsInWorld())
            continue;  // fully offline members can't be meeting-stone summoned anyway

        // GET_PLAYERBOT_AI is non-null for self-bot ("player botAI") humans too -- they get a
        // shell AI with IsRealPlayer() == true. Only skip actual bots.
        PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);
        if (memberBotAI && !memberBotAI->IsRealPlayer())
            continue;  // playerbot, not a real player

        if (member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId())
        {
            lastSeenOnMap[slot.guid] = now;
            continue;
        }

        // Per-member grace: measure from when this member was last seen inside the bot's
        // instance, so someone stepping out mid-run gets a fresh window instead of an instant
        // summon popup. Members never seen inside fall back to the bot's own entry time.
        auto seen = lastSeenOnMap.find(slot.guid);
        time_t const leftAt = seen != lastSeenOnMap.end() ? seen->second : botEnteredAt;
        if (!leftAt || now - leftAt < time_t(sPlayerbotAIConfig.lfgLatecomerGraceSeconds))
            continue;

        return member;
    }

    return nullptr;
}
