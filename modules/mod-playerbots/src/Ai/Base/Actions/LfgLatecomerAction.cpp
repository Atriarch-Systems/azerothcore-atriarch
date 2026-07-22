/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgLatecomerAction.h"

#include "Config.h"
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

    time_t const now = time(nullptr);

    // Drop backoff entries whose time has passed so the map stays bounded over a long uptime.
    for (auto it = summonRetryAt.begin(); it != summonRetryAt.end();)
    {
        if (it->second <= now)
            it = summonRetryAt.erase(it);
        else
            ++it;
    }

    // A summon offer is already pending on this player's screen (ours from an earlier pass, or
    // e.g. a warlock's) -- using the stone again would just re-send SMSG_SUMMON_REQUEST and reset
    // their 2-minute accept window, which is exactly the popup spam this guards against.
    if (latecomer->GetSummonExpireTimer() > now)
        return false;

    // A previous offer's window ran out with the player still outside (decline and ignore are
    // indistinguishable server-side) -- hold off before offering again. Entries past their time
    // were pruned above, so any remaining entry means "still backing off".
    if (summonRetryAt.find(latecomerGuid) != summonRetryAt.end())
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

    // Inline config read cached in a function-local static; a later consolidation pass will move
    // this into PlayerbotAIConfig.
    static uint32 const summonRetryMinutes =
        sConfigMgr->GetOption<uint32>("AiPlayerbot.Lfg.SummonRetryMinutes", 5);

    // Don't offer this player another summon until the 2-minute accept window of the offer just
    // sent (MAX_PLAYER_SUMMON_DELAY, Player.h) has expired AND the retry backoff has passed.
    summonRetryAt[latecomerGuid] =
        now + time_t(MAX_PLAYER_SUMMON_DELAY) + time_t(summonRetryMinutes * MINUTE);

    LOG_INFO("playerbots", "Bot {} <{}>: used meeting stone to summon late group member {}",
             bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), latecomer->GetName().c_str());
    return true;
}

bool LfgSummonLatecomerAction::isUseful() { return AI_VALUE(Unit*, "lfg latecomer") != nullptr; }
