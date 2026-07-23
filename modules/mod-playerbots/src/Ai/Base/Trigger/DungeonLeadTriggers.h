/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONLEADTRIGGERS_H
#define PLAYERBOTS_DUNGEONLEADTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Fires for the one bot that is its dungeon group's navigation driver
// (PlayerbotAI::GetDungeonNavigationLeader(), docs/dungeon-progression-driver.md), so
// DungeonLeadMoveAction can walk the run toward its next boss. Real 3s checkInterval - the paired
// action commits movement legs that take multiples of that to walk, so per-tick checking would be
// pure waste (docs/playerbot-performance.md item 3).
class DungeonLeadNavigationTrigger : public Trigger
{
public:
    DungeonLeadNavigationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon lead navigation", 3) {}

    bool IsActive() override;
};

#endif
