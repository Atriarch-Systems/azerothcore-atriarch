/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LFGDUNGEONCOMPLETEACTION_H
#define PLAYERBOTS_LFGDUNGEONCOMPLETEACTION_H

#include "Action.h"

class PlayerbotAI;

// Paired with LfgDungeonCompleteTrigger (LfgTriggers.h) via the "lfg" strategy: once the group's
// LFG state is LFG_STATE_FINISHED_DUNGEON (final encounter down), the run's driver says one
// completion line in party chat, then every unattended random bot lingers for
// AiPlayerbot.LfgCompleteLingerSeconds plus a stable per-bot 0-60s spread (guid-derived, so the
// group trickles out instead of vanishing in the same tick) and leaves the group. The core's
// LFGScripts::OnRemoveMember teleports a member who leaves an LFG group out of the dungeon
// (Player::TeleportToEntryPoint) and, in the finished state, skips both the deserter debuff and
// the offer-continue, so the plain group leave IS the full "player finished the run" exit - no
// separate teleport-out packet needed (docs/dungeon-progression-driver.md).
//
// Per-instance one-shot state below follows LfgLeaderAnnounceAction's scheme: a different map or
// instance id re-arms both the announcement and the linger clock, so one persistent action object
// per bot handles run after run with no explicit reset hook.
class LfgDungeonCompleteAction : public Action
{
public:
    LfgDungeonCompleteAction(PlayerbotAI* botAI) : Action(botAI, "lfg dungeon complete") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Which run (group + map + instance) the finished state was first observed in, and when;
    // 0 until the first observation. The group counter is part of the key because instance ids
    // are eventually recycled - a later run of the same map/instance pair must not inherit a
    // stale linger clock.
    uint32 completeGroupCounter = 0;
    uint32 completeMapId = 0;
    uint32 completeInstanceId = 0;
    uint32 completeSince = 0;
    bool announced = false;
};

#endif
