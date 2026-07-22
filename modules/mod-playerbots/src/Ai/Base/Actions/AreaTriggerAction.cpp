/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AreaTriggerAction.h"

#include "DBCStores.h"
#include "Event.h"
#include "LastMovementValue.h"
#include "LFGMgr.h"
#include "Map.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Transport.h"

bool ReachAreaTriggerAction::Execute(Event event)
{
    if (botAI->IsRealPlayer())  // Do not trigger own area trigger.
        return false;

    uint32 triggerId;
    WorldPacket p(event.getPacket());
    p.rpos(0);
    p >> triggerId;

    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
    if (!at)
        return false;

    AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(triggerId);
    if (!teleport)
    {
        WorldPacket p1(CMSG_AREATRIGGER);
        p1 << triggerId;
        p1.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p1);

        return true;
    }

    if (bot->GetMapId() != at->map)
    {
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "area_trigger_follow_too_far_error", "I won't follow: too far away", {}));
        return true;
    }

    // Don't chase the master out of an active LFG dungeon run through this
    // specific trigger if its teleport target would take the bot off the
    // current dungeon map to a non-dungeon destination (e.g. an exit portal).
    // Triggers that keep the bot on the same map, that lead further into a
    // dungeon (another dungeon-flagged destination map), or that fire when no
    // active run is in progress (finished/disbanded), are left untouched.
    if (Map* map = bot->GetMap())
    {
        if (map->IsDungeon() && teleport->target_mapId != bot->GetMapId())
        {
            MapEntry const* targetMapEntry = sMapStore.LookupEntry(teleport->target_mapId);
            bool targetIsDungeon = targetMapEntry && targetMapEntry->IsDungeon();
            if (!targetIsDungeon)
            {
                if (Group* group = bot->GetGroup())
                {
                    if (sLFGMgr->GetState(group->GetGUID()) == lfg::LFG_STATE_DUNGEON)
                    {
                        botAI->TellMaster("I'll hold here - dungeon run's still active.");
                        return true;
                    }
                }
            }
        }
    }

    bot->GetMotionMaster()->MovePoint(
        /*id*/ at->map,
        /*coords*/ at->x, at->y, at->z,
        /*forcedMovement*/ FORCED_MOVEMENT_NONE,
        /*speed*/ 0.0f,             // default speed (not handled here)
        /*orientation*/ 0.0f,       // keep current orientation of bot
        /*generatePath*/ true,      // true => terrain path (2d mmap); false => straight spline (3d vmap)
        /*forceDestination*/ false);

    float distance = bot->GetDistance(at->x, at->y, at->z);
    float delay = 1000.0f * distance / bot->GetSpeed(MOVE_RUN) + sPlayerbotAIConfig.reactDelay;
    botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "area_trigger_wait_for_me", "Wait for me", {}));
    botAI->SetNextCheckDelay(delay);
    context->GetValue<LastMovement&>("last area trigger")->Get().lastAreaTrigger = triggerId;

    return true;
}

bool AreaTriggerAction::Execute(Event /*event*/)
{
    LastMovement& movement = context->GetValue<LastMovement&>("last area trigger")->Get();

    uint32 triggerId = movement.lastAreaTrigger;
    movement.lastAreaTrigger = 0;

    if (!sObjectMgr->GetAreaTrigger(triggerId))
        return false;

    if (!sObjectMgr->GetAreaTriggerTeleport(triggerId))
        return true;

    WorldPacket p(CMSG_AREATRIGGER);
    p << triggerId;
    p.rpos(0);
    bot->GetSession()->HandleAreaTriggerOpcode(p);

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));
    return true;
}
