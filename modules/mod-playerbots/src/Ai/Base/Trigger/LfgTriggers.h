/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGTRIGGERS_H
#define PLAYERBOTS_LFGTRIGGERS_H

#include "Trigger.h"

#include <ctime>

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

// Fires for an LFG group's leader bot once it has been inside a dungeon instance for at least
// AiPlayerbot.Lfg.LatecomerGraceSeconds and a real (non-playerbot) group member still hasn't
// zoned in. Paired with LfgSummonLatecomerAction via the "lfg" strategy (LfgStrategy.cpp). See
// docs/dungeon-leadership-and-summon.md, section 3a.
//
// The 30s checkInterval below doubles as the "don't re-attempt more than once every 30s" cooldown
// called for by the design doc -- Trigger::needCheck() (Trigger.cpp) already skips IsActive()
// entirely between checks, so no separate cooldown bookkeeping is needed.
//
// "Time since this bot entered its current instance" is tracked here as plain members on this
// Trigger instance rather than a static/global guid-keyed map: one Trigger object already exists
// per bot for the lifetime of its "lfg" strategy (created once via TriggerContext's factory), so a
// member timestamp is already bot-scoped. It is recomputed (and implicitly cleared) whenever the
// bot's current map/instance id no longer matches what was last observed, which also covers "bot
// left the map".
class LfgLatecomerTrigger : public Trigger
{
public:
    LfgLatecomerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "lfg latecomer", 30 * 1000) {}

    bool IsActive() override;
    std::string const GetTargetName() override { return "lfg latecomer"; }

private:
    uint32 lastMapId = 0;
    uint32 lastInstanceId = 0;
    time_t enteredInstanceAt = 0;
};

#endif
