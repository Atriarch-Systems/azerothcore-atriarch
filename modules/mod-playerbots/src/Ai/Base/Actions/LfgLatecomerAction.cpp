/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgLatecomerAction.h"

#include "Event.h"
#include "MeetingStoneSummonHelper.h"
#include "Playerbots.h"

// A reasonable meeting-stone search radius around the bot -- generous enough to cover the
// entrance area of most 5-man dungeons without scanning the whole zone.
static constexpr float LFG_MEETING_STONE_SEARCH_RANGE = 40.0f;

bool LfgSummonLatecomerAction::Execute(Event /*event*/)
{
    Unit* latecomerUnit = AI_VALUE(Unit*, "lfg latecomer");
    if (!latecomerUnit)
        return false;

    Player* latecomer = latecomerUnit->ToPlayer();
    if (!latecomer)
        return false;

    // Trigger::Check() and this Execute() run synchronously within the same AI tick, so holding
    // `latecomerUnit` this long is safe -- but re-resolve by guid anyway before doing anything
    // that changes state, on general principle (this codebase's convention is to never trust a
    // Player*/Unit* handed across any call boundary without a fresh lookup).
    ObjectGuid latecomerGuid = latecomer->GetGUID();
    latecomer = ObjectAccessor::FindPlayer(latecomerGuid);
    if (!latecomer || !latecomer->IsInWorld())
        return false;

    GameObject* meetingStone =
        MeetingStoneSummonHelper::FindNearestRealMeetingStone(bot, LFG_MEETING_STONE_SEARCH_RANGE);
    if (!meetingStone)
    {
        LOG_INFO("playerbots",
                 "Bot {} <{}>: no meeting stone found nearby to summon late group member {}",
                 bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), latecomer->GetName().c_str());
        return false;
    }

    if (!MeetingStoneSummonHelper::SummonPlayerViaMeetingStone(bot, latecomer, meetingStone))
        return false;

    LOG_INFO("playerbots", "Bot {} <{}>: used meeting stone to summon late group member {}",
             bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), latecomer->GetName().c_str());
    return true;
}

bool LfgSummonLatecomerAction::isUseful() { return AI_VALUE(Unit*, "lfg latecomer") != nullptr; }
