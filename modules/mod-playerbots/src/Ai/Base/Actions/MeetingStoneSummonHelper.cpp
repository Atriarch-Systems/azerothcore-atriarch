/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MeetingStoneSummonHelper.h"

#include "CellImpl.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "NearestGameObjects.h"
#include "Player.h"

namespace MeetingStoneSummonHelper
{
    GameObject* FindNearestRealMeetingStone(Player* bot, float range)
    {
        if (!bot)
            return nullptr;

        // Same scan shape as SummonAction::SummonUsingGos (UseMeetingStoneAction.cpp), adapted to
        // pick the nearest match instead of the first one found.
        std::list<GameObject*> targets;
        AnyGameObjectInObjectRangeCheck check(bot, range);
        Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, targets, check);
        Cell::VisitObjects(bot, searcher, range);

        GameObject* nearest = nullptr;
        float nearestDist = range;
        for (GameObject* go : targets)
        {
            if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_MEETINGSTONE)
                continue;

            float dist = bot->GetDistance(go);
            if (!nearest || dist < nearestDist)
            {
                nearest = go;
                nearestDist = dist;
            }
        }

        return nearest;
    }

    bool SummonPlayerViaMeetingStone(Player* bot, Player* target, GameObject* meetingStone)
    {
        if (!bot || !target || !meetingStone)
        {
            LOG_DEBUG("playerbots", "SummonPlayerViaMeetingStone: invalid bot/target/meetingStone pointer");
            return false;
        }

        if (!target->IsInWorld())
        {
            LOG_DEBUG("playerbots", "SummonPlayerViaMeetingStone: target {} is not in world",
                      target->GetGUID().ToString().c_str());
            return false;
        }

        // Mirrors GameObject.cpp's GAMEOBJECT_TYPE_MEETINGSTONE case: "accept only use by player
        // from same raid as caster" -- IsInSameRaidWith() for a non-raid group just means "same
        // Group*" (Player.h).
        if (!target->IsInSameRaidWith(bot))
        {
            LOG_DEBUG("playerbots", "SummonPlayerViaMeetingStone: {} is not in the same group as {}",
                      target->GetName().c_str(), bot->GetName().c_str());
            return false;
        }

        GameObjectTemplate const* info = meetingStone->GetGOInfo();
        if (!info)
        {
            LOG_DEBUG("playerbots", "SummonPlayerViaMeetingStone: meeting stone {} has no template",
                      meetingStone->GetGUID().ToString().c_str());
            return false;
        }

        if (bot->GetLevel() < info->meetingstone.minLevel || target->GetLevel() < info->meetingstone.minLevel)
        {
            LOG_DEBUG("playerbots",
                      "SummonPlayerViaMeetingStone: level requirement ({}) not met by bot {} or target {}",
                      info->meetingstone.minLevel, bot->GetLevel(), target->GetLevel());
            return false;
        }

        // Reproduce a real player's right-click on the stone: select the target, then let the
        // unmodified core GameObject::Use() path do everything else (spell 23598 ->
        // SMSG_SUMMON_REQUEST). Note Player::SetTarget() is a no-op override ("used for serverside
        // target changes, does not apply to players" -- Player.h) -- SetSelection() is the real
        // setter for UNIT_FIELD_TARGET, which is what GameObject.cpp's meeting-stone case reads via
        // player->GetTarget().
        bot->SetSelection(target->GetGUID());
        meetingStone->Use(bot);
        return true;
    }
}
