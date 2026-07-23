/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonLeadAction.h"

#include "Creature.h"
#include "CreatureData.h"
#include "Event.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool DungeonLeadMoveAction::isUseful()
{
    if (botAI->GetDungeonNavigationLeader() != bot)
        return false;

    if (bot->IsInCombat() || !bot->IsAlive())
        return false;

    return PartyReady();
}

// Pause the drive - without giving the action up entirely - while the party isn't in a state to
// take the next pull. Relevance deliberately does NOT handle this (the driver at 3.5 outranks the
// UseFood 3.0 eat/drink actions); this gate is what actually defers to resting.
bool DungeonLeadMoveAction::PartyReady() const
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Map* map = bot->GetMap();

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != map)
            continue;

        // A dead member means the group stops and handles it (bots' own rez/corpse-run logic, a
        // real player's spirit run) - never advances past them. A released ghost that ran far
        // from the group is also caught by the gap check below once it's alive again.
        if (!member->IsAlive())
            return false;

        if (bot->GetDistance(member) > sPlayerbotAIConfig.dungeonLeadMaxPartyGap)
            return false;

        // Resource gate applies to bots only: a real player's mana bar is their own business,
        // and stalling the run because the human hasn't drunk to 70% would read as a hang.
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI || memberAI->IsRealPlayer())
            continue;

        if (member->GetHealthPct() < sPlayerbotAIConfig.dungeonLeadRestThreshold)
            return false;

        if (member->getPowerType() == POWER_MANA)
        {
            uint32 maxMana = member->GetMaxPower(POWER_MANA);
            if (maxMana && 100.0f * member->GetPower(POWER_MANA) / maxMana <
                               sPlayerbotAIConfig.dungeonLeadRestThreshold)
                return false;
        }
    }

    return true;
}

// Objective selection: nearest alive, non-friendly CREATURE_FLAG_EXTRA_DUNGEON_BOSS creature on
// this map, from the spawn-id creature store - no per-dungeon route data required, works in every
// instance including raids. Known cuts (docs/dungeon-progression-driver.md "Known limitations"):
// summoned encounter bosses aren't DB spawns so they can't be objectives until they exist, and a
// dungeon whose bosses lack the flag simply never drives - i.e. exactly today's behavior.
Creature* DungeonLeadMoveAction::FindNearestAliveBoss() const
{
    Map* map = bot->GetMap();
    if (!map)
        return nullptr;

    Creature* best = nullptr;
    float bestDist = 0.0f;

    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* creature = pair.second;
        if (!creature || !creature->IsInWorld() || !creature->IsAlive())
            continue;

        if (!creature->IsDungeonBoss())
            continue;

        if (creature->IsFriendlyTo(bot))
            continue;

        float dist = bot->GetDistance(creature);
        if (!best || dist < bestDist)
        {
            best = creature;
            bestDist = dist;
        }
    }

    return best;
}

bool DungeonLeadMoveAction::Execute(Event /*event*/)
{
    Creature* boss = FindNearestAliveBoss();
    if (!boss)
        return false;  // all bosses dead (run complete) or none flagged - nothing to drive toward

    // Close enough: stop issuing movement and let proximity aggro start the fight. Deliberately
    // inside any even-level boss's aggro radius so arrival always transitions into combat rather
    // than the group forming a semicircle just outside it.
    if (bot->GetDistance(boss) < 10.0f)
        return false;

    // Let a previously committed leg finish before re-pathing. Same rationale as the equivalent
    // gate in NewRpgBaseAction::MoveFarTo (which see, at length): MoveTo caps its stored delay at
    // maxWaitForMove, so on a long leg this action re-enters mid-walk, and re-issuing MovePoint
    // from the new position makes mmaps' partial-path endpoint jump around - the bot oscillates
    // instead of walking. Returning true (not false) keeps the engine treating the drive as the
    // consumed action for this tick instead of falling through to lower-relevance movement.
    LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
    if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
    {
        float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
        if (remaining > 10.0f)
            return true;
    }

    return MoveTo(bot->GetMapId(), boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
}
