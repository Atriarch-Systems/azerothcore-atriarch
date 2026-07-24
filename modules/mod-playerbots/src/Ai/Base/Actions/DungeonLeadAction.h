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
    bool PartyReadyOrStallTimedOut();

    // Stall-timeout bookkeeping (AiPlayerbot.DungeonLeadStallTimeoutSeconds): one action object
    // exists per bot for the lifetime of its AiObjectContext, so these members persist across
    // trigger fires. notReadySince is the getMSTime() at which PartyReady() first went false
    // (0 while ready); the map/instance pair records where that was observed so a new instance
    // starts with a clean slate.
    uint32 notReadySince = 0;
    uint32 notReadyMapId = 0;
    uint32 notReadyInstanceId = 0;
    bool stallOverride = false;
};

#endif
