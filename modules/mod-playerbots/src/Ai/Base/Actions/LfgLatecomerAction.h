/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGLATECOMERACTION_H
#define PLAYERBOTS_LFGLATECOMERACTION_H

#include "Action.h"
#include "ObjectGuid.h"

#include <ctime>
#include <map>

class PlayerbotAI;

// Paired with LfgLatecomerTrigger (LfgTriggers.h/.cpp) via the "lfg" strategy. Resolves the real
// group member the trigger found waiting outside the instance (shared via the "lfg latecomer"
// AI_VALUE, matching e.g. PartyMemberToHeal/ReachPartyMemberToHealAction's convention) and, if a
// real meeting stone is nearby, summons them in through the real core mechanic. See
// docs/dungeon-leadership-and-summon.md, section 3a.
//
// Two rate limits on top of the trigger's 30s cadence keep this from spamming the player with a
// summon popup every pass:
//  - while a summon offer is still pending on the player (Player::GetSummonExpireTimer() in the
//    future), no new summon is sent -- re-using the stone would re-send SMSG_SUMMON_REQUEST and
//    reset the player's 2-minute accept window;
//  - once an offer's window has run out with the player still outside (decline and ignore are
//    indistinguishable server-side), the next offer to that player is held back for
//    AiPlayerbot.Lfg.SummonRetryMinutes, tracked per member guid in summonRetryAt. One action
//    object exists per bot for the lifetime of its AiObjectContext, so the map is bot-scoped.
class LfgSummonLatecomerAction : public Action
{
public:
    LfgSummonLatecomerAction(PlayerbotAI* botAI) : Action(botAI, "lfg summon latecomer") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // member guid -> earliest time a fresh summon may be offered to them again
    std::map<ObjectGuid, time_t> summonRetryAt;
};

#endif
