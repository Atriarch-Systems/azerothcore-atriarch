/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONLEADSTRATEGY_H
#define PLAYERBOTS_DUNGEONLEADSTRATEGY_H

#include "PassThroughStrategy.h"

// Carrier for the dungeon progression driver (docs/dungeon-progression-driver.md). Attached to
// every random bot by AiFactory - the trigger and action self-gate on being the group's actual
// navigation leader, so handing the strategy to everyone costs a couple of pointer checks per
// trigger interval for bots it doesn't apply to.
class DungeonLeadStrategy : public PassThroughStrategy
{
public:
    // Relevance 3.5: above the ungated "new rpg go ..." wander/grind actions (3.0) that would
    // otherwise win inside instances and walk the leader off the run, below looting (6.0) and
    // grind's own eat/drink reflexes (4.1-4.2). Deferring to party rest is the action's readiness
    // gate's job, not relevance's.
    DungeonLeadStrategy(PlayerbotAI* botAI) : PassThroughStrategy(botAI, 3.5f) {}

    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dungeon lead"; }
};

#endif
