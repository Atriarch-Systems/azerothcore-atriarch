/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGLATECOMERACTION_H
#define PLAYERBOTS_LFGLATECOMERACTION_H

#include "Action.h"

class PlayerbotAI;

// Paired with LfgLatecomerTrigger (LfgTriggers.h/.cpp) via the "lfg" strategy. Resolves the real
// group member the trigger found waiting outside the instance (shared via the "lfg latecomer"
// AI_VALUE, matching e.g. PartyMemberToHeal/ReachPartyMemberToHealAction's convention) and, if a
// real meeting stone is nearby, summons them in through the real core mechanic. See
// docs/dungeon-leadership-and-summon.md, section 3a.
class LfgSummonLatecomerAction : public Action
{
public:
    LfgSummonLatecomerAction(PlayerbotAI* botAI) : Action(botAI, "lfg summon latecomer") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
