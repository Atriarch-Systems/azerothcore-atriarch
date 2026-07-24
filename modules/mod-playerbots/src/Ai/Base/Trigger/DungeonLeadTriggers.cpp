/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonLeadTriggers.h"

#include "LFGMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool DungeonLeadNavigationTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.dungeonLeadNavigation)
        return false;

    // Non-combat engine only runs out of combat, but the trigger can be checked on the boundary -
    // don't queue a movement leg that combat will immediately clobber.
    if (bot->IsInCombat())
        return false;

    if (botAI->GetDungeonNavigationLeader() != bot)
        return false;

    // A finished LFG run (group state LFG_STATE_FINISHED_DUNGEON, set by LFGMgr::FinishDungeon()
    // when the final encounter completes) has nothing left to drive toward - stop re-pathing and
    // let the "lfg dungeon complete" flow (announce, linger, leave) own the endgame.
    if (Group* group = bot->GetGroup())
        if (sLFGMgr->GetState(group->GetGUID()) == lfg::LFG_STATE_FINISHED_DUNGEON)
            return false;

    return true;
}
