/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgValues.h"

#include "Group.h"
#include "Playerbots.h"

Unit* LfgLatecomerValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (slot.guid == bot->GetGUID())
            continue;

        Player* member = ObjectAccessor::FindPlayer(slot.guid);
        if (!member || !member->IsInWorld())
            continue;  // fully offline members can't be meeting-stone summoned anyway

        if (GET_PLAYERBOT_AI(member))
            continue;  // playerbot, not a real player

        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return member;
    }

    return nullptr;
}
