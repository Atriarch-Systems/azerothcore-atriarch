/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONLEADACTION_H
#define PLAYERBOTS_DUNGEONLEADACTION_H

#include "MovementActions.h"

class PlayerbotAI;

// The forward-motion half of the dungeon progression driver
// (docs/dungeon-progression-driver.md): the group's navigation leader
// (PlayerbotAI::GetDungeonNavigationLeader()) walks toward the nearest alive dungeon boss, one
// mmaps movement leg per "dungeon lead navigation" trigger fire, pausing while the party is dead,
// spread out, or resting. Everything encounter-shaped along the way - trash aggro, the boss's own
// proximity aggro at the end - is left to the ordinary combat engine.
class DungeonLeadMoveAction : public MovementAction
{
public:
    DungeonLeadMoveAction(PlayerbotAI* botAI) : MovementAction(botAI, "dungeon lead move") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Creature* FindNearestAliveBoss() const;
    bool PartyReady() const;
};

#endif
