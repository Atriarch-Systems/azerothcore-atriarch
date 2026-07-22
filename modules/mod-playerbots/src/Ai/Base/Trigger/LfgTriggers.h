/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGTRIGGERS_H
#define PLAYERBOTS_LFGTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

class LfgProposalActiveTrigger : public Trigger
{
public:
    LfgProposalActiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg proposal active", 20 * 2000) {}

    bool IsActive() override;
};

class UnknownDungeonTrigger : public Trigger
{
public:
    UnknownDungeonTrigger(PlayerbotAI* botAI) : Trigger(botAI, "unknown dungeon", 20 * 2000) {}

    bool IsActive() override;
};

// Fires for an LFG group's leader bot while it is inside a dungeon instance and a real
// (non-playerbot) group member has been outside that instance for at least
// AiPlayerbot.Lfg.LatecomerGraceSeconds. Paired with LfgSummonLatecomerAction via the "lfg"
// strategy (LfgStrategy.cpp). See docs/dungeon-leadership-and-summon.md, section 3a.
//
// The 30s checkInterval below doubles as the "don't re-attempt more than once every 30s" cooldown
// called for by the design doc -- Trigger::needCheck() (Trigger.cpp) already skips IsActive()
// entirely between checks, so no separate cooldown bookkeeping is needed.
//
// All grace-period bookkeeping (per-member "last seen inside my instance" timestamps, with the
// bot's own instance-entry time as fallback for members never seen inside) lives in
// LfgLatecomerValue (LfgValues.h) so the trigger, the action's isUseful() and the action's
// Execute() all agree on the same per-member grace decision.
class LfgLatecomerTrigger : public Trigger
{
public:
    LfgLatecomerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg latecomer", 30 * 1000) {}

    bool IsActive() override;
    std::string const GetTargetName() override { return "lfg latecomer"; }
};

#endif
