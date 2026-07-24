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

// Fires for a bot leading an LFG group inside a dungeon while at least one real player is in the
// group, so the leader can announce - exactly once per group+instance - that it will lead unless
// asked to hand leadership over (docs/bot-economy.md, Phase 1f). Same leader-only + dungeon-map
// gating as LfgLatecomerTrigger above. The one-shot bookkeeping (and its reset when the group or
// instance changes) lives in LfgLeaderAnnounceAction, keyed by group+instance, so this trigger
// stays stateless and just keeps offering the cheap condition; the action no-ops after it has
// said its line. Paired via the "lfg" strategy (LfgStrategy.cpp).
class LfgLeaderAnnounceTrigger : public Trigger
{
public:
    LfgLeaderAnnounceTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg leader announce", 10 * 1000) {}

    bool IsActive() override;
};

// Fires for every grouped bot inside a dungeon once the LFG run is complete - LFGMgr::
// FinishDungeon() has set the GROUP's state to LFG_STATE_FINISHED_DUNGEON (final encounter down,
// reward handed out). Paired with LfgDungeonCompleteAction via the "lfg" strategy so random bots
// announce the clear, linger a bit, then teleport out and leave like a player would
// (docs/dungeon-progression-driver.md). 30s interval: end-of-run bookkeeping has no reason to be
// checked more often, and the action's linger window is minutes long anyway.
class LfgDungeonCompleteTrigger : public Trigger
{
public:
    LfgDungeonCompleteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg dungeon complete", 30000) {}

    bool IsActive() override;
};

#endif
