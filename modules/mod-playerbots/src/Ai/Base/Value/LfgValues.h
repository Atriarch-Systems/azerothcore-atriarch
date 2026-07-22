/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGVALUES_H
#define PLAYERBOTS_LFGVALUES_H

#include "Value.h"

class PlayerbotAI;

class LfgProposalValue : public ManualSetValue<uint32>
{
public:
    LfgProposalValue(PlayerbotAI* botAI) : ManualSetValue<uint32>(botAI, 0, "lfg proposal") {}
};

// Finds a real (non-playerbot) group member who has not yet zoned into the bot's current
// map/instance. Shared by LfgLatecomerTrigger and LfgSummonLatecomerAction (docs/dungeon-leadership-
// and-summon.md, section 3a) so both agree on the same target within a tick, the same convention
// used by e.g. PartyMemberToHeal/ReachPartyMemberToHealAction.
class LfgLatecomerValue : public UnitCalculatedValue
{
public:
    LfgLatecomerValue(PlayerbotAI* botAI) : UnitCalculatedValue(botAI, "lfg latecomer", 2 * 1000) {}

protected:
    Unit* Calculate() override;
};

#endif
