/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGVALUES_H
#define PLAYERBOTS_LFGVALUES_H

#include "ObjectGuid.h"
#include "Value.h"

#include <ctime>
#include <map>

class PlayerbotAI;

class LfgProposalValue : public ManualSetValue<uint32>
{
public:
    LfgProposalValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0, "lfg proposal") {}
};

// Finds a real (non-playerbot) group member who has been outside the bot's current map/instance
// for at least AiPlayerbot.Lfg.LatecomerGraceSeconds. Shared by LfgLatecomerTrigger and
// LfgSummonLatecomerAction (docs/dungeon-leadership-and-summon.md, section 3a) so both agree on
// the same target within a tick, the same convention used by e.g.
// PartyMemberToHeal/ReachPartyMemberToHealAction.
//
// The grace period is tracked per member: a small map of "last seen inside my map/instance"
// timestamps (refreshed on every Calculate() while the member IS inside) so a member who steps
// out mid-run gets a fresh grace window measured from when they actually left, instead of being
// summoned on the very next trigger check. Members never seen inside fall back to the time the
// bot itself entered the instance. All tracking state resets whenever the bot's map/instance
// changes (which also covers "bot left the dungeon"), the same scheme the trigger used to keep
// for its single bot-entry timestamp. One value object exists per bot for the lifetime of its
// AiObjectContext, so plain members are already bot-scoped.
class LfgLatecomerValue : public UnitCalculatedValue
{
public:
    LfgLatecomerValue(PlayerbotAI* botAI) : UnitCalculatedValue(botAI, "lfg latecomer", 2 * 1000) {}

protected:
    Unit* Calculate() override;

private:
    void ResetTracking();

    uint32 lastMapId = 0;
    uint32 lastInstanceId = 0;
    time_t botEnteredAt = 0;
    std::map<ObjectGuid, time_t> lastSeenOnMap;
};

#endif
